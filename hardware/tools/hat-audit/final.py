import sys,copy,math,pickle
sys.path.insert(0,'.')
from plan import *
from rats import score
def gbox(P,refs):
    E=[ext(P[r]) for r in refs]; return (min(e[0] for e in E),max(e[1] for e in E),min(e[2] for e in E),max(e[3] for e in E))
def place(P,refs,rot,x0,ytop):
    move(P,refs,rot=rot); b=gbox(P,refs); move(P,refs,dx=x0-b[0],dy=ytop-b[3])
PLAN=[
 ('A','CAN3 controller',['U7','C26','C27','C28'],0,38.0,-28.0,'Out of the bottom band, under the SPI pins'),
 ('B','40 MHz clock',['X1','C29','R20','R21','R22'],90,48.6,-47.6,'Into the space U7 leaves, between the chips it feeds'),
 ('C','3V3 regulator',['U4','C15','C16','C17','C18','C19'],180,43.0,-55.2,'From the top band to the middle of its loads'),
 ('D','LED level shifter',['U11','C45'],90,54.0,-24.0,'Below J1, beside R39 and D19'),
 ('E','Reset button',['SW1'],0,47.0,-4.0,'Beside the watchdog in the top band'),
 ('F','Current-sense resistor',['R27'],0,22.4,-27.5,'Next to C33 and U8, out of the switch parts'),
]
def build():
    P=copy.deepcopy(P0); M=[]
    for k,name,refs,rot,x0,yt,why in PLAN:
        b0=gbox(P,refs); c0={r:P[r]['c'] for r in refs}
        place(P,refs,rot,x0,yt)
        M.append(dict(k=k,name=name,refs=refs,rot=rot,why=why,b0=b0,b1=gbox(P,refs),c0=c0,c1={r:P[r]['c'] for r in refs}))
    return P,M
if __name__=='__main__':
    P,M=build()
    c0=check(P0); print('new overlaps',[b for b in check(P) if b not in c0])
    t0,x0,per0,_=score(P0); t,x,per,_=score(P)
    print('current  MST %.0f X %d pwr %.0f'%(t0,x0,per0['+3V3']+per0['5V_MAIN'])); print('proposed MST %.0f X %d pwr %.0f'%(t,x,per['+3V3']+per['5V_MAIN']))
    for n in per:
        if abs(per[n]-per0[n])>2: print(f'  {n:14} {per0[n]:6.1f} -> {per[n]:6.1f}')
    print(len(j1cross(P)),j1cross(P))
    from moves import swapped
    Q,new=swapped(P); t,x,_,_=score(Q); print('plus swaps MST %.0f X %d'%(t,x), len(j1cross(Q)))
    for m in M:
        print(m['k'],m['name'],'rot',m['rot'],[round(v,1) for v in m['b0']],'->',[round(v,1) for v in m['b1']])
        for r in m['refs']: print(f"    {r:4} ({m['c0'][r][0]:6.2f},{m['c0'][r][1]:7.2f}) -> ({m['c1'][r][0]:6.2f},{m['c1'][r][1]:7.2f})")
    pickle.dump((P,M),open('final.pkl','wb'))
