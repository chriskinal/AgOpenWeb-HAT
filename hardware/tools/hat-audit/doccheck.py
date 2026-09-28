import re,sys,collections
sys.path.insert(0,'.')
from sch import load_sch
import os
DOC=os.path.join(os.path.dirname(os.path.abspath(__file__)),'..','..','hat','HARDWARE_HAT_NETLIST.md')
doc=open(DOC).read()
exp={}
def add(p,n): exp[p]=n
def conn(name):
    blk=doc.split(f'**{name}**')[1].split('\n\n')[1]
    for m in re.finditer(r'\| (\d+) \| ([^|]+) \| \| (\d+) \| ([^|]+) \|',blk):
        for a,b in ((m.group(1),m.group(2)),(m.group(3),m.group(4))):
            n=b.strip(); n=re.sub(r'\s*\(.*\)','',n).strip('`')
            add(f'{name}.{a}',n)
conn('J1'); conn('J3')
for j,txt in (('J2','1 `GLOBAL_EN`, 2 GND, 3 `RUN_PG`'),('J8','1 GND, 2 `CON_TX`, 3 `CON_RX`')):
    for m in re.finditer(r'(\d+) `?([\w+]+)`?',txt): add(f'{j}.{m.group(1)}',m.group(2))
for m in re.finditer(r'^\| (`[^`]+`|GND|no connect) \| ([^|]*\.[^|]*) \|$',doc,re.M):
    net=m.group(1).strip('`')
    for tok in re.split(r'[;,]\s*',m.group(2)):
        tok=tok.strip()
        mm=re.fullmatch(r'([A-Z]+)(\d+)\.(\w+)–\1(\d+)\.\3',tok)
        if mm:
            for i in range(int(mm.group(2)),int(mm.group(4))+1): add(f'{mm.group(1)}{i}.{mm.group(3)}',net)
            continue
        mm=re.fullmatch(r'([A-Z]+\d+)\.(\d+)–(\d+)',tok)
        if mm:
            for i in range(int(mm.group(2)),int(mm.group(3))+1): add(f'{mm.group(1)}.{i}',net)
            continue
        if re.fullmatch(r'[A-Z]+\d+\.\w+',tok): add(tok,net)
for u in ('U5','U6','U7'):
    for p in (2,6,7,8,12,17,18,20,26,27): add(f'{u}.{p}','no connect')
parts,pins,_=load_sch()
# map doc pin-letter to sch pin by pin NAME where letters
act={}
byname=collections.defaultdict(dict)
for (r,n),v in pins.items():
    net=next(iter(v['nets'])) if v['nets'] else 'no connect'
    act[f'{r}.{n}']=net; byname[r][v['name'].upper()]=net
passive=lambda r: re.match(r'(R|C)\d',r) or r in('D9','D10','D11','D12','D13','D14','SJ1','SJ2','SJ3','BZ1','TV1','TV2')
errs=[];seen=set()
for p,en in exp.items():
    r,pin=p.split('.')
    if r not in parts: errs.append(f'{r} not in schematic'); continue
    if passive(r) and pin in('1','2'):
        if r in seen: continue
        seen.add(r)
        pe={exp.get(f'{r}.1'),exp.get(f'{r}.2')}; pa={act.get(f'{r}.1'),act.get(f'{r}.2')}
        if pe!=pa: errs.append(f'{r}: expected {pe} got {pa}')
        continue
    if pin in 'KAGDS':
        an=byname[r].get(pin)
        if an is None: an=f'?names={byname[r]}'
    else: an=act.get(p,'MISSING PIN')
    if an!=en: errs.append(f'{p}: expected {en} got {an}')
print('doc pins',len(exp),'sch pins',len(act))
print('sch pins not in doc:',sorted(k for k in act if k not in exp and not (k.split(".")[1] in ("1","2","3") and any(x.startswith(k.split(".")[0]+".") for x in exp) and not k.split(".")[0].startswith(("U","J")))))
print('refs in sch not doc:',sorted(r for r in parts if not any(k.startswith(r+'.') for k in exp)))
print('MISMATCH',len(set(errs)));[print(' ',e) for e in sorted(set(errs))]
