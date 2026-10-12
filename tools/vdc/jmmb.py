import re
def jm_mbs(path, poc_frame=0):
    """per-MB syntax for the first picture in the JM trace"""
    mbs = []; cur = None; pic = -1
    for l in open(path, errors='replace'):
        m = re.match(r'\*+ POC: (\d+) .*MB: *(\d+) Slice', l)
        if m:
            n = int(m.group(2))
            if n == 0: pic += 1
            if pic > poc_frame: break
            cur = {'mb': n, 'i4': []}; 
            if pic == poc_frame: mbs.append(cur)
            continue
        if cur is None: continue
        m = re.match(r'@\d+\s+(\S.*?)\s{2,}\S+\s+\(\s*(-?\d+)\)', l)
        if not m: continue
        k, v = m.group(1).strip(), int(m.group(2))
        if k.startswith('intra4x4_pred_mode'): cur['i4'].append(v)
        elif k in ('mb_type', 'intra_chroma_pred_mode', 'coded_block_pattern', 'mb_qp_delta', 'mb_skip_run'):
            cur[k] = v
    return mbs
