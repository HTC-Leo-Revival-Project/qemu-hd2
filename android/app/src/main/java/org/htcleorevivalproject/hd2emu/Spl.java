package org.htcleorevivalproject.hd2emu;

import android.content.Context;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;

/**
 * BYON (bring your own NAND): the two SPLs the app ships as fallbacks and what it
 * does with them.
 *
 * <ul>
 * <li>{@code assets/spl/spl_stock.bin}: the stock HTC SPL (a RUU's section 0x200).  What a
 *     stock RUU puts on the phone; it enforces the CID and only flashes signed NBHs.</li>
 * <li>{@code assets/spl/hspl.bin}: the HSPL, the Cotulla-patched SPL that boots anything and
 *     flashes any NBH from the card.  The emulator's SPL LLE mode runs it out of NAND.</li>
 * </ul>
 *
 * The user's own base NAND is built from a STOCK RUU (never a custom one): its radio, its OS
 * area and its SPL.  The Settings option "Replace SPL with HSPL" then swaps the SPL inside
 * the selected image, and the bundled stock SPL puts it back.
 */
public final class Spl {
    private Spl() {
    }

    private static byte[] asset(Context c, String name) throws IOException {
        InputStream in = c.getAssets().open("spl/" + name);
        try {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            return out.toByteArray();
        } finally {
            in.close();
        }
    }

    public static byte[] hspl(Context c) throws IOException {
        return asset(c, "hspl.bin");
    }

    public static byte[] stock(Context c) throws IOException {
        return asset(c, "spl_stock.bin");
    }

    /** Swaps the SPL inside an existing NAND image (it must be a whole 512 MiB one). */
    public static void replace(File image, byte[] spl) throws IOException {
        Nand nand = Nand.edit(image);
        try {
            nand.flashSpl(spl);
            nand.writeHtcConfig();
        } finally {
            nand.close();
        }
    }

    /**
     * The minimal NAND: just enough to boot the SPL and flash from it -- the SPL, the
     * boot chain's config records and the factory MACs.  Put a LEOIMG.nbh on the SD card
     * and the real SPL flashes it, the way it does on the phone.
     */
    public static void createMinimal(File out, byte[] spl) throws IOException {
        Nand nand = new Nand(out);
        try {
            nand.writeFactoryMacs(Nand.mac("00:23:76:48:44:32"), Nand.mac("00:23:76:48:44:33"));
            nand.flashSpl(spl);
            nand.writeHtcConfig();
        } finally {
            nand.close();
        }
    }
}
