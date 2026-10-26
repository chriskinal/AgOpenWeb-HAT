# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The AgOpenWeb-HAT contributors

import sys,copy,math
sys.path.insert(0,'.')
from plan import *
from rats import mst,netpts
def occupied(P,skip):
    boxes=[]
    for r,p in P.items():
        if r in skip: continue
        if p['pads'] and all(q['layer']=='2' for q in p['pads']): continue
        boxes.append(ext(p))
    return boxes
def fits(b,boxes,m=0.3):
    if b[0]<0.8 or b[1]>64.2 or b[3]>-0.8 or b[2]<-71.7: return False
    # keep 1.4 mm escape strip round J1 handled by J1 box + margin
    for B in boxes:
        if b[0]<B[1]+m and B[0]<b[1]+m and b[2]<B[3]+m and B[2]<b[3]+m: return False
    for h in holes:
        ko=(5.5 if h['d']<3 else 6.5)/2
        cx=min(max(h['x'],b[0]),b[1]); cy=min(max(h['y'],b[2]),b[3])
        if math.dist((h['x'],h['y']),(cx,cy))<ko: return False
    return True
def local_len(P,refs,w={'GND':0.0,'+3V3':0.5,'5V_MAIN':0.5}):
    N=netpts(P); tot=0
    touched={q['net'] for r in refs for q in P[r]['pads'] if q['net']}
    for n in touched:
        if n in N and len(N[n])>1: tot+=w.get(n,1.0)*mst([p[:2] for p in N[n]])[0]
    return tot
def search(P,refs,step=1.0,top=8):
    boxes=occupied(P,refs); boxes.append((6.5,58.5,-23.4,-16.2))  # J1 + escape strip
    base=local_len(P,refs)
    E=[ext(P[r]) for r in refs]; b0=(min(e[0] for e in E),max(e[1] for e in E),min(e[2] for e in E),max(e[3] for e in E))
    res=[]
    for rot in (0,90,180,270):
        Q=copy.deepcopy(P); move(Q,refs,rot=rot)
        E=[ext(Q[r]) for r in refs]; b=(min(e[0] for e in E),max(e[1] for e in E),min(e[2] for e in E),max(e[3] for e in E))
        x=1.0
        while x+(b[1]-b[0])<64.5:
            y=-1.0
            while y-(b[3]-b[2])>-72:
                nb=(x,x+b[1]-b[0],y-(b[3]-b[2]),y)
                if fits(nb,boxes):
                    R=copy.deepcopy(Q); move(R,refs,dx=x-b[0],dy=y-b[3])
                    res.append((local_len(R,refs),rot,round(x,1),round(y,1),nb))
                y-=step
            x+=step
    res.sort(key=lambda r:r[0])
    return base,b0,res[:top]
if __name__=='__main__':
    groups={'WDT':['U10','R37','C43','C44','R38'],'LDO':['U4','C15','C16','C17','C18','C19'],'LEDBUF':['U11','C45'],'SW1':['SW1'],
      'RS232':['U9','C34','C35','C36','C37','C38','C39'],'OSC':['X1','C29','R20','R21','R22'],
      'CAN1':['U5','C20','C21','C22'],'CAN2':['U6','C23','C24','C25'],'CAN3':['U7','C26','C27','C28'],
      'PZDRV':['Q10','R40','R41'],'KEYSENSE':['R3','R4','C5','D2'],'VINSENSE':['R16','R17','C14','D5'],'KEYDIV':['R5','R6','C6'],
      'HOLD':['R9','D4','C8','R10'],'ADC':['U8','C30','C31','C32','C33','R26','R27','R19']}
    sel=sys.argv[1:] or list(groups)
    for g in sel:
        base,b0,res=search(P0,groups[g],step=1.0)
        print(g,'now %.0f at box'%base,[round(v,1) for v in b0])
        for r in res[:5]: print('    %.0f rot%d box x%.1f..%.1f y%.1f..%.1f'%(r[0],r[1],r[4][0],r[4][1],r[4][2],r[4][3]))
