import re,sys
g=[[' ']*64 for _ in range(20)]
r=c=0
data=sys.stdin.read()
i=0
while i < len(data):
    m=re.match(r'\x1b\[(\d+);(\d+)H', data[i:])
    if m:
        r=int(m.group(1))-1; c=int(m.group(2))-1; i+=m.end(); continue
    if data[i:i+4]=='\x1b[2J':
        g=[[' ']*64 for _ in range(20)]; i+=4; continue
    if data[i:i+3]=='\x1b[H':
        r=c=0; i+=3; continue
    ch=data[i]; i+=1
    if ch=='\n': r=min(19,r+1); c=0; continue
    if ch=='\r' or ch=='\x1b': continue
    if 0<=r<20 and 0<=c<64: g[r][c]=ch
    c+=1
for n,row in enumerate(g): print(f"{n+1:2}|{''.join(row)}|")
