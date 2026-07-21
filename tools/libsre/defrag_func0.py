import json
from capstone import *
BASE=0x30000000
data=open("prx/libsre.linked.bin","rb").read()
md=Cs(CS_ARCH_PPC, CS_MODE_32|CS_MODE_BIG_ENDIAN)
seeds=json.load(open("prx/libsre.func0.minimal.json"))
rows=sorted((int(f['start'],16),int(f['end'],16)) for f in seeds)
COVERED_END=max(e for s,e in rows)
COVERED_LO=min(s for s,e in rows)

# --- instruction map over all seed code: addr -> (size, branch_targets, falls_through) ---
insmap={}
bl_targets=set()
for s,e in rows:
    for ins in md.disasm(data[s-BASE:e-BASE], s):
        mn=ins.mnemonic; sz=ins.size; tgts=[]; ft=True
        if mn=='bl':
            try: bl_targets.add(int(ins.op_str,16))
            except: pass
        elif mn=='b':
            try: tgts=[int(ins.op_str,16)]
            except: pass
            ft=False                      # unconditional branch: no fall-through
        elif mn in ('blr','bctr'):
            ft=False                      # return / computed jump: terminal
        elif mn in ('bctrl','bclr'):
            pass                          # link forms fall through
        elif mn[0]=='b' and mn not in ('bl',):
            # conditional branch bc/bne/beq/bge/... : target is last operand
            try: tgts=[int(ins.op_str.split(',')[-1].strip(),16)]
            except: pass
        insmap[ins.address]=(sz,tgts,ft)
bl_targets={t for t in bl_targets if COVERED_LO<=t<COVERED_END}

def cfg_extent(entry, limit):
    """max end address reachable from entry via fall-through + intra branches, < limit."""
    seen=set(); stack=[entry]; maxend=entry
    while stack:
        a=stack.pop()
        if a in seen or a<entry or a>=limit or a not in insmap: continue
        seen.add(a); sz,tgts,ft=insmap[a]; maxend=max(maxend,a+sz)
        if ft: stack.append(a+sz)
        for t in tgts:
            if entry<=t<limit: stack.append(t)
    return maxend

# entries = all seed starts + any bl-target (forced). Sorted.
starts=sorted(set(s for s,e in rows) | bl_targets)
n=len(starts); result=[]; i=0
while i<n:
    entry=starts[i]
    ext=cfg_extent(entry, COVERED_END)
    j=i+1
    # absorb successors the CFG reaches, but NEVER cross a bl-target (hard boundary)
    while j<n and starts[j]<ext and starts[j] not in bl_targets:
        # re-extend: the absorbed fragment may branch further (multi-level)
        ext=max(ext, cfg_extent(entry, COVERED_END))
        j+=1
    end = starts[j] if j<n else COVERED_END
    end = min(end, ext) if False else min(end, ext)  # clip to CFG extent
    # but ensure at least the original seed end for this entry (don't shrink real code)
    orig_end=max([e for s,e in rows if s==entry] or [entry])
    end=max(end, orig_end) if end<orig_end else end
    result.append((entry, max(end, entry+4)))
    i=j
result.sort()
# stitch gaps: extend each fn end to the next fn start (no holes -> lifter covers all code)
stitched=[]
for k,(s,e) in enumerate(result):
    nxt=result[k+1][0] if k+1<len(result) else COVERED_END
    stitched.append((s, min(max(e,s+4), nxt) if e<nxt else nxt))
# fill remaining gaps by extending to next start
final=[]
for k,(s,e) in enumerate(stitched):
    nxt=stitched[k+1][0] if k+1<len(stitched) else COVERED_END
    final.append((s, nxt))   # contiguous partition by entry
overlaps=sum(1 for k in range(len(final)-1) if final[k][1]>final[k+1][0])
print(f"seeds {len(rows)} -> {len(final)}  bl_targets={len(bl_targets)}  overlaps={overlaps}")
for name,a in [("30014A58",0x30014A58),("3000A000",0x3000A000),("30014CF8",0x30014CF8)]:
    fn=[(s,e) for s,e in final if s==a]
    print(f"  0x{name}: {'0x%X->0x%X'%fn[0] if fn else 'MISSING'}")
json.dump([{"start":"0x%x"%s,"end":"0x%x"%e} for s,e in final], open("prx/libsre.func0.defrag.json","w"), indent=0)
print("wrote func0.defrag.json")
