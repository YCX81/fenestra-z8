"""Sparse read-only UI snapshots, plus qualified external eye level input.

No guest memory write or CPU-register redirection. Main firmware is exact G.
"""
import json
import struct

last = -10
input_at = [(2, '1'), (14, '0'), (22, '1'), (30, '0'), (38, '1')]
inputs_sent = 0
def sample(g, ns):
    global last, inputs_sent
    vt = ns / 1e9
    if inputs_sent < len(input_at) and vt >= input_at[inputs_sent][0]:
        when, level = input_at[inputs_sent]
        g.qmp.command('qom-set',path='/machine',property='eye-gpio-input',value=level)
        g.data.setdefault('eye_inputs',[]).append(dict(requested_at=when,actual_at=vt,level=int(level)))
        inputs_sent += 1
    if vt < last + 2:
        return dict(skipped=True)
    last = vt
    result = dict(vt=vt,eye=json.loads(g.qmp.command('qom-get',path='/machine',property='eye-gpio-state')))
    rd=lambda a,n:int.from_bytes(g.read_bytes(a,n),'little')
    valid=lambda a,n=1:0xa00000000<=a<=0xa40000000-n and a%8==0
    def fields(a,n):
        q=g.read_bytes(a,n)
        return q,lambda o,w=4:int.from_bytes(q[o:o+w],'little')
    try:
        native_view=rd(0xa032280c8,8)
        result['view']=hex(native_view)
        if valid(native_view,0x118):
            _,v=fields(native_view,0x118)
            result['view_id_state']=[v(0x54),v(0xd0)]
        result['output'] = 4 if rd(0xa03229eb0,1) else 1
        result['output_active']=rd(0xa032c55fc,4)
        result['eye_driver']=g.read_bytes(0xa02e815c8,32).hex()
        root=0xa03229ee0+4*0x118
        q,r=fields(root,0x118)
        result['root']=dict(id=r(0),bounds=list(struct.unpack_from('<4h',q,0x10)),
            tuple=[r(0x108),r(0x10c),r(0x110)],children=r(0xd8),callback=hex(r(0x48,8)))
        children=[];node=r(0xc8,8)
        for _ in range(min(r(0xd8),32)):
            if not valid(node,24):break
            n,nv=fields(node,24); child=nv(16,8)
            if not valid(child,0xc0):break
            c,cv=fields(child,0xc0)
            children.append(dict(address=hex(child),id=cv(0),flags=cv(0xc),
                bounds=list(struct.unpack_from('<4h',c,0x10)),callback=hex(cv(0x48,8))))
            node=nv(8,8)
        result['children']=children
        group,output,target=result['root']['tuple']
        if group==1 and output in (1,2,4) and target<=4:
            desc=0xa02f35f98+(0 if output==1 else 1 if output==2 else 2)*0x1938+0xb0+target*0x208
            z,d=fields(desc,0x208)
            result['descriptor']=dict(address=hex(desc),width=d(8,2),height=d(10,2),backing=hex(d(0x28,8)),
                format=d(0x3c),enabled=d(0x4c,1),rect=list(struct.unpack_from('<4H',z,0x1f4)))
        pages=rd(0xa032b50d8,8)
        if valid(pages,664*96):
            core=rd(pages+663*96+80,8)
            if valid(core,2536):
                registry=rd(core+2528,8)
                if valid(registry,24) and rd(registry,4)==0x47525846:
                    ui=rd(registry+8,8)
                    if valid(ui,37976):
                        raw,u=fields(ui,37976);ds=12072
                        result['ui']=dict(address=hex(ui),magic=hex(u(0)),status=u(40),loaded=u(32),
                            engine=hex(u(8,8)),attached=u(ds+224),frames=u(ds+228),callbacks=u(ds+232),error=u(ds+240),
                            width=u(ds+216),height=u(ds+220),viewport=list(struct.unpack_from('<4H',raw,ds+6032)),
                            target=[u(ds+6056+x*4) for x in range(3)],surface=hex(u(ds+200,8)),
                            application=hex(u(ds+6016,8)),gate=u(18144,8),pose=u(37880),
                            icon_retry=u(37876),icon_blocked=u(37904),icon_backing=hex(u(37912,8)),
                            presents=u(18456),present_fail=u(18464),not_current=u(18468),target_lost=u(37952))
                        (g.out/f'ui-{vt:06.2f}.bin').write_bytes(raw)
        with (g.out/'eye-ui.jsonl').open('a') as f:f.write(json.dumps(result)+'\n')
        if int(vt)//4 != int(vt-2)//4:
            image=g.out/f'eye-{vt:06.2f}.ppm'
            g.qmp.command('screendump',filename=str(image))
        return result
    except Exception as e:
        result['observation_error']=repr(e)
        with (g.out/'eye-ui.jsonl').open('a') as f:f.write(json.dumps(result)+'\n')
        return result
