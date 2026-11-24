"""crest_puzzle.py: stage 5 room 21's sliding puzzle (the water prison's control board) solved by BFS, and the bot's
input for it (scripts/story/27_crest_puzzle.txt): the cursor moves cell by cell to the next occupied cell, CROSS held
grabs the panel under it and a direction slides it one cell. Prints the steps."""
from collections import deque
P=[('W0',2,1,0,0),('W1',2,1,2,0),('W2',2,1,4,0),('L',1,2,0,1),('V0',1,2,1,1),('s0',1,1,2,1),('s1',1,1,3,1),('V1',1,2,4,1),('R',1,2,5,1),('W3',2,1,0,3),('W4',2,1,4,3)]
names=[p[0] for p in P]; size={p[0]:(p[1],p[2]) for p in P}
start=tuple((p[3],p[4]) for p in P)
def occ(st):
    g={}
    for n,(c,r) in zip(names,st):
        w,h=size[n]
        for dc in range(w):
            for dr in range(h): g[(c+dc,r+dr)]=n
    return g
def key(st):
    d={}
    for n,pos in zip(names,st): d.setdefault((size[n],n if n in('L','R') else ''),[]).append(pos)
    return tuple(sorted((k,tuple(sorted(v))) for k,v in d.items()))
DIRS={'R':(1,0),'L':(-1,0),'D':(0,1),'U':(0,-1)}
goal=lambda st: st[names.index('L')]==(2,2) and st[names.index('R')]==(3,2)
q=deque([(start,[])]); seen={key(start)}; sol=None
while q:
    st,path=q.popleft()
    if goal(st): sol=path; break
    g=occ(st)
    for i,n in enumerate(names):
        c,r=st[i]; w,h=size[n]
        for dn,(dc,dr) in DIRS.items():
            nc,nr=c+dc,r+dr
            if nc<0 or nr<0 or nc+w>6 or nr+h>4: continue
            if not all(g.get((nc+x,nr+y),n)==n for x in range(w) for y in range(h)): continue
            ns=list(st); ns[i]=(nc,nr); ns=tuple(ns)
            if key(ns) in seen: continue
            seen.add(key(ns)); q.append((ns,path+[(i,dn)]))
BTN={'R':'RIGHT','L':'LEFT','D':'DOWN','U':'UP'}
def hop(st,cur,d):
    g=occ(st); c,r=st[cur]; dc,dr=DIRS[d]
    while True:
        c,r=c+dc,r+dr
        if not(0<=c<6 and 0<=r<4): return cur
        n=g.get((c,r))
        if n and names.index(n)!=cur: return names.index(n)
def step(st,cell,d):
    g=occ(st); c,r=cell; dc,dr=DIRS[d]
    while True:
        c,r=c+dc,r+dr
        if not(0<=c<6 and 0<=r<4): return cell
        if (c,r) in g: return (c,r)
lines=[]; st=list(start); cur=(0,3)
for i,d in sol:
    g=occ(tuple(st)); target={cell for cell,n in g.items() if n==names[i]}
    q=deque([(cur,[])]); seen2={cur}; path=None
    while q:
        u,p=q.popleft()
        if u in target: path=(u,p); break
        for dd in 'RLDU':
            v=step(tuple(st),u,dd)
            if v not in seen2: seen2.add(v); q.append((v,p+[dd]))
    if path is None: raise SystemExit('cannot reach '+names[i])
    cell,p=path
    for dd in p: lines += ['press %s 6' % BTN[dd], 'wait 18']
    lines += ['cursor %d %d' % cell, 'slide CROSS %s 32' % BTN[d], 'wait 20']
    c,r=st[i]; st[i]=(c+DIRS[d][0],r+DIRS[d][1]); cur=(cell[0]+DIRS[d][0],cell[1]+DIRS[d][1])
print(chr(10).join(lines))
