import re,sys,collections
import os
DOC=os.path.join(os.path.dirname(os.path.abspath(__file__)),'..','..','HARDWARE_HAT_NETLIST.md')
doc=open(DOC).read()
# ---- expected: pin -> net
exp={}
def add(p,n): exp[p]=n
def conn(name):
    blk=doc.split(f'**{name}**')[1].split('\n\n')[1]
    for m in re.finditer(r'\| (\d+) \| ([^|]+) \| \| (\d+) \| ([^|]+) \|',blk):
        for a,b in ((m.group(1),m.group(2)),(m.group(3),m.group(4))):
            add(f'{name}.{a}',b.strip().strip('`'))
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
# pin-name -> number
SOT23={'G':'1','S':'2','D':'3'}
PINMAP={}
def num(p):
    r,pin=p.split('.')
    if pin in('K','A'): return f'{r}.'+('1' if pin=='K' else '2')
    if pin in('G','D','S'): return f'{r}.'+PINMAP.get(r,SOT23)[pin]
    return p
E={num(p):n for p,n in exp.items()}

# ---- actual
t=open(sys.argv[1]).read()
parts={m.group(1):(m.group(2),m.group(3)) for m in re.finditer(r'\[\n([^\n]*)\n([^\n]*)\n([^\n]*)\n',t)}
act={}
for n,b in re.findall(r'\(\n([^\n]*)\n((?:[^\n()]+\n)*)\)',t):
    for p in b.split(): act[p.replace('-','.')]=n.strip()
ALIAS=dict(a.split('=') for a in sys.argv[2:])  # ref renames e.g. CN1=J3
act={ (ALIAS.get(p.split('.')[0],p.split('.')[0])+'.'+p.split('.')[1]):n for p,n in act.items()}
present={ALIAS.get(r,r) for r in parts}
passive=lambda r: re.match(r'(R|C)\d',r) or r in('D9','D10','D11','D12','D13','D14','SJ1','SJ2','SJ3','BZ1')
# compare net membership (net names compared too, but report name diffs separately)
errs=[]; names=collections.Counter()
checked=set()
for p,en in E.items():
    r=p.split('.')[0]
    if r not in present: continue
    an=act.get(p)
    if passive(r):
        other=p[:-1]+('2' if p.endswith('1') else '1')
        pair_e={en,E.get(other)}; pair_a={act.get(p) or 'no connect',act.get(other) or 'no connect'}
        if other in checked: continue
        checked.add(p)
        if pair_e!=pair_a: errs.append(f'{r}: expected {sorted(pair_e)} got {sorted(pair_a)}')
        continue
    an=an or 'no connect'
    if an!=en: errs.append(f'{p}: expected {en} got {an}')
extra=[p for p in act if p.split('.')[0] in present and p not in E]
print('parts in export:',len(parts),'| checked refs:',len([r for r in present if any(k.startswith(r+'.') for k in E)]))
print('unknown refs:',sorted(r for r in present if not any(k.startswith(r+'.') for k in E)))
print('pins not in doc:',extra)
print('MISMATCHES (%d):'%len(errs)); [print('  ',e) for e in errs]
