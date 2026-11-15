import sys,json,time
sys.path.insert(0,'.')
import livectl
O=json.load(open('push/uioffs44.json'))
t,p=livectl.connect()
rd=lambda a,n:int.from_bytes(bytes(p.debug.read(a,n)),'little')
root=rd(0xa032b50c8+16,8);state=rd(root+663*96+80,8);u=rd(state+2528,8)
if rd(u,4)==0x47525846:u=rd(u+8,8)
eng=rd(u+8,8)
W8={'last_present_us','icon_fallback_until','gate'}
def snap():
    s={k:rd(u+o,8 if k in W8 else 4) for k,o in O.items() if k!='d_object'}
    s['display_status']=hex(s['display_status']);s['photo_movie']=rd(0xa03bd062c,4);s['requested']=rd(eng+1728,4)
    s['enc']=rd(eng+12272,8);s['ignored']=rd(eng+12360,8);s['tx']=rd(eng+1952,4);s['press']=rd(eng+1760,8)
    return s
a=snap();print(json.dumps(a));time.sleep(5);b=snap()
print('delta/5s',{k:(b[k]-a[k]) for k in b if isinstance(b[k],int) and b[k]!=a[k]})
