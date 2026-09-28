import json,collections,re,os
SCH=os.environ.get('HAT_SCH',os.path.expanduser('~/Downloads/SCH_AOW-HAT_2026-09-26.json'))
def load_sch():
    d=json.load(open(SCH))
    pins={}  # (ref,num)->dict
    parts={}
    issues=[]
    for sh in d['schematics']:
        title=sh['title']
        flags=[];plist=[]
        for s in sh['dataStr']['shape']:
            if s.startswith('F~'):
                seg=s.split('^^'); h=seg[0].split('~')
                kind=h[1]; x,y=float(h[2]),float(h[3])
                name=seg[2].split('~')[0]
                flags.append((x,y,name,kind,h[5]))
            elif s.startswith('LIB') and 'frame_lib' not in s.split('#@$')[0]:  # skip the sheet frame
                P=s.split('#@$'); h=P[0].split('~')
                attrs=h[3].split('`'); a=dict(zip(attrs[0::2],attrs[1::2]))
                ref=None;val=None
                for p in P[1:]:
                    q=p.split('~')
                    if q[0]=='T' and q[1]=='P': ref=q[12]
                    if q[0]=='T' and q[1]=='N': val=q[12]
                if ref is None: continue
                parts[ref]=dict(sheet=title,val=val,pkg=a.get('package'),lcsc=a.get('Supplier Part'),mpn=a.get('Manufacturer Part'),attrs=a)
                for p in P[1:]:
                    if p.startswith('P~'):
                        seg=p.split('^^'); q=seg[0].split('~')
                        x,y=float(q[4]),float(q[5])
                        name=seg[3].split('~')[4]; num=seg[4].split('~')[4]
                        plist.append((ref,num,name,x,y,q[3]))
            elif not s.startswith(('F~','LIB')): issues.append((title,s[:80]))
        fl=collections.defaultdict(list)
        for f in flags: fl[(round(f[0],1),round(f[1],1))].append(f)
        pl=collections.defaultdict(list)
        for p in plist: pl[(round(p[3],1),round(p[4],1))].append(p)
        used=set()
        for k,ps in pl.items():
            nets={f[2] for f in fl.get(k,[])}
            if k in fl: used.add(k)
            for p in ps:
                pins[(p[0],p[1])]=dict(name=p[2],nets=nets,sheet=title,xy=k,others=[o[0]+'.'+o[1] for o in ps if o is not p],spice=p[5])
        for k,f in fl.items():
            if k not in used: issues.append((title,'dangling flag',k,[x[2] for x in f]))
    return parts,pins,issues
if __name__=='__main__':
    parts,pins,issues=load_sch()
    print(len(parts),'parts',len(pins),'pins')
    print('issues',issues)
    for k,v in sorted(pins.items()):
        if len(v['nets'])!=1: print(k,v)
    c=collections.Counter(p['sheet'] for p in parts.values()); print(c)
