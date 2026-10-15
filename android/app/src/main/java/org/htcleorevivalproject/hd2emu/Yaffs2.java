package org.htcleorevivalproject.hd2emu;

import java.nio.charset.Charset;


import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

/**
 * The filesystem half of the NAND builder: the tree a ROM zip turns into, and
 * the yaffs2 image the kernel would have written for it.  A direct port of
 * the yaffs2 writer in tools/hd2_mknand.py.
 */
class Yaffs2 {

/** ASCII-flavoured: byte values have to survive the round trip. */
static final Charset LATIN1 = Charset.forName("ISO-8859-1");
static final Charset UTF8 = Charset.forName("UTF-8");

    static final int FILE = 1;
    static final int SYMLINK = 2;
    static final int DIRECTORY = 3;

    static final int S_IFREG = 0100000;
    static final int S_IFLNK = 0120000;
    static final int S_IFDIR = 0040000;

    static class Node {
        String name;
        String kind;                    // dir | file | link
        int mode = 0644;
        int uid, gid;
        int mtime;
        byte[] data = new byte[0];
        String target = "";
        final TreeMap<String, Node> children = new TreeMap<String, Node>();

        Node(String name, String kind) {
            this.name = name;
            this.kind = kind;
        }
    }

    static class Tree {
        final Node root = new Node("", "dir");

        Tree() {
            root.mode = 0755;
        }

        Node lookup(String path) {
            Node n = root;
            for (String p : path.replaceAll("^/+", "").split("/")) {
                if (p.isEmpty()) continue;
                Node c = n.children.get(p);
                if (c == null) return null;
                n = c;
            }
            return n;
        }

        Node lookup(String path, boolean createDirs) {
            Node n = root;
            for (String p : path.replaceAll("^/+", "").split("/")) {
                if (p.isEmpty()) continue;
                Node c = n.children.get(p);
                if (c == null) {
                    if (!createDirs) return null;
                    c = new Node(p, "dir");
                    c.mode = 0755;
                    n.children.put(p, c);
                }
                n = c;
            }
            return n;
        }

        void addFile(String path, byte[] data, int mode, int mtime) {
            int slash = path.lastIndexOf('/');
            Node parent = lookup(slash < 0 ? "" : path.substring(0, slash), true);
            Node f = new Node(path.substring(slash + 1), "file");
            f.mode = mode;
            f.mtime = mtime;
            f.data = data;
            parent.children.put(f.name, f);
        }

        void addDir(String path, int mode, int mtime) {
            Node n = lookup(path, true);
            n.mode = mode;
            n.mtime = mtime;
        }

        void symlink(String target, String path) {
            int slash = path.lastIndexOf('/');
            Node parent = lookup(slash < 0 ? "" : path.substring(0, slash), true);
            Node l = new Node(path.substring(slash + 1), "link");
            l.mode = 0777;
            l.mtime = (int) (System.currentTimeMillis() / 1000);
            l.target = target;
            parent.children.put(l.name, l);
        }

        List<Node> walk(Node n, String path) {
            List<Node> out = new ArrayList<Node>();
            out.add(n);
            for (Map.Entry<String, Node> e : n.children.entrySet()) {
                Node c = e.getValue();
                String p = path + "/" + e.getKey();
                if ("dir".equals(c.kind)) out.addAll(walk(c, p));
                else out.add(c);
            }
            return out;
        }
    }

    static void setPerm(Node n, int uid, int gid, int mode) {
        n.uid = uid;
        n.gid = gid;
        n.mode = (n.mode & ~07777) | mode;
    }

    static void setPermRecursive(Tree tree, int uid, int gid, int dmode, int fmode, String path) {
        Node top = tree.lookup(path);
        if (top == null) return;
        for (Node n : tree.walk(top, path)) {
            if ("dir".equals(n.kind)) setPerm(n, uid, gid, dmode);
            else if ("file".equals(n.kind)) setPerm(n, uid, gid, fmode);
        }
    }

    /** Writes the tree into the flash, the way the kernel's yaffs2 would. */
    static class Writer {
        private final Nand nand;
        private final int first;
        private final int nblocks;
        private int block, chunk;
        private int seq = 0x1000;          // YAFFS_LOWEST_SEQUENCE_NUMBER
        private int nextId = 257;           // YAFFS_NOBJECT_BUCKETS + 1

        Writer(Nand nand, int firstBlock, int blocks) {
            this.nand = nand;
            this.first = firstBlock;
            this.nblocks = blocks;
        }

        private void write(byte[] data, int objId, int chunkId, int nBytes) throws IOException {
            if (block >= nblocks) throw new IOException("yaffs2: partition full");
            long page = (long) (first + block) * Nand.PPB + chunk;
            nand.writePage(page, data, seq, objId, chunkId, nBytes);
            if (++chunk == Nand.PPB) {
                chunk = 0;
                block++;
                seq++;
            }
        }

        private void header(int objId, int parent, Node node, int otype, int size)
                throws IOException {
            byte[] h = Nand.fill(Nand.PAGE, (byte) 0xff);
            byte[] name = node.name.getBytes(LATIN1);
            if (name.length > 255) name = Arrays.copyOf(name, 255);
            byte[] alias = node.target.getBytes(LATIN1);
            if (alias.length > 159) alias = Arrays.copyOf(alias, 159);

            int mode = node.mode;
            mode |= otype == FILE ? S_IFREG : otype == SYMLINK ? S_IFLNK : S_IFDIR;

            java.nio.ByteBuffer b = java.nio.ByteBuffer.wrap(h).order(java.nio.ByteOrder.LITTLE_ENDIAN);
            b.putInt(0, otype);
            b.putInt(4, parent);
            System.arraycopy(name, 0, h, 10, name.length);
            b.putInt(268, mode);
            b.putInt(272, node.uid);
            b.putInt(276, node.gid);
            b.putInt(280, node.mtime);
            b.putInt(284, node.mtime);
            b.putInt(288, node.mtime);
            b.putInt(292, size);
            b.putInt(296, 0);
            System.arraycopy(alias, 0, h, 300, alias.length);

            /* the extra header info goes in the tags, as yaffs_update_oh does */
            write(h, objId | (otype << 28), 0x80000000 | parent,
                    otype == FILE ? size : 0);
        }

        /** @return how many blocks the tree took */
        int writeTree(Tree tree) throws IOException {
            emit(tree.root, 1);
            return block + (chunk > 0 ? 1 : 0);
        }

        private void emit(Node node, int parent) throws IOException {
            for (Map.Entry<String, Node> e : node.children.entrySet()) {
                Node c = e.getValue();
                int oid = nextId++;
                if ("dir".equals(c.kind)) {
                    header(oid, parent, c, DIRECTORY, 0);
                    emit(c, oid);
                } else if ("link".equals(c.kind)) {
                    header(oid, parent, c, SYMLINK, 0);
                } else {
                    header(oid, parent, c, FILE, c.data.length);
                    for (int i = 0; i < c.data.length; i += Nand.PAGE) {
                        int n = Math.min(Nand.PAGE, c.data.length - i);
                        byte[] p = Nand.fill(Nand.PAGE, (byte) 0xff);
                        System.arraycopy(c.data, i, p, 0, n);
                        write(p, oid, i / Nand.PAGE + 1, n);
                    }
                }
            }
        }
    }

    private static final class Arrays {
        static byte[] copyOf(byte[] in, int n) {
            byte[] out = new byte[n];
            System.arraycopy(in, 0, out, 0, Math.min(n, in.length));
            return out;
        }
    }
}
