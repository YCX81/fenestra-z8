#!/usr/bin/env python3
"""只读解析 v3 W5Fnnnnn.BIN；不以最后一条有效记录冒充确定挂点。"""
import argparse,struct,json
from pathlib import Path
NAMES=['准备写入候选','关闭后/REC调用任务','service open 返回','REC bring-up 返回','adapter start 入口','adapter wait 前','OPEN 返回','CONFIGURE 返回','REGISTER 返回','SET_PARAMS 返回','HEADERS 返回','RUN 返回','旧版发送前(禁用)','adapter完成通知调用已返回','adapter回调入口(RAM)','hand-back 调用前入口','门控拒绝/epoch变化标记']
EVENTS=(0,15,1,16,2,13)
REASONS={1:'epoch变化(仅标记)',2:'shutdown',4:'arm≠generation/arm=0',8:'ready=0',16:'claimed/入口已消耗',32:'照片模式',64:'写锁争用/独占失败',128:'CAS后复查失败'}
STAGES=['无失败阶段','guard','SUBMIT','STOP','首次 TRIGGER','picture 等待','drain TRIGGER','worker=5 等待','FINISH 后释放检查','EXIT 后 VDI 检查','busy 等待','close-lock 等待']
def checksum(b):
 h=2166136261
 for v in b:h=((h^v)*16777619)&0xffffffff
 return h

def decode(data):
 rows=[];errors=[]
 if len(data)!=3072:errors.append(f'文件长度 {len(data)}，预期 v3 3072；可能创建未完成/截断')
 for offset in range(0,len(data)-511,512):
  b=data[offset:offset+512]
  if not any(b):continue
  p=struct.unpack('<128I',b)
  if p[0]!=0x33524657 or p[1]!=3 or checksum(b[:508])!=p[127]:
   errors.append(f'扇区 {offset//512}：魔数/版本/校验失败，不解释为记录');continue
  if offset//512>=len(EVENTS) or p[2]!=offset//512 or p[3]!=EVENTS[offset//512]:errors.append(f'扇区 {offset//512}：序号/事件非法');continue
  light=p[3] in (0,15,16)
  details=[dict(事件=p[i],阶段=p[i+1],错误=hex(p[i+2]),写入=p[i+3],关闭=hex(p[i+4]),编号=p[i+5]) for i in range(28,64,6)] if p[3] in (15,16) else None
  if light:
   p=list(p);arm=p[87];p[28:117]=[0]*89;p[87]=arm
  q=lambda i:p[i]|p[i+1]<<32
  rows.append(dict(序号=p[2],事件=NAMES[p[3]],事件号=p[3],地址=hex(p[4]),计数器=q(6),会话=hex(q(8)),
   客户=p[10],返回值=hex(p[11]) if p[3] in (2,3,6,7,8,9,10,11) else '不适用',
   原始x0=hex(q(12)),原始x19=hex(q(14)),原始LR=hex(q(16)),
   准备IO错误=hex(p[18]),准备写入字节=p[19],准备close=hex(p[20]),卡代次=p[21],丢弃掩码=hex(p[26]),
   REC代次=p[118],到达掩码=hex(p[27]),事务阶段快照=list(p[119:125]),hand_back已返回=bool(p[125]&256) or p[3] in (1,2,13),轻量记录=light,诊断事务详情=details,原因掩码=hex(p[125]),原因=[v for k,v in REASONS.items() if p[125]&k],当前runtime_epoch=p[126],shutdown=p[5],arm=p[87],请求kbps=p[22],请求大小=p[23],请求arena=hex(q(24)),
   Live有效=p[28]==0x57354c31,fin_rc=hex(p[29]),失败阶段=p[29]>>24,
   失败阶段说明=STAGES[p[29]>>24] if p[29]>>24<len(STAGES) else '未知',close_failed=p[30],close_lock=p[31],Live流=p[32],Live忙=p[33],release_req=p[34],
   closes=p[35],eos_ok=p[36],preempts=p[37],Live_arena=hex(q(38)),原关闭耗时微秒=p[40],eos_miss=p[41],帧数=p[42],历史阶段掩码=hex(p[43]),
   workers=list(p[44:46]),服务槽=[list(p[46:49]),list(p[49:52])],core忙=list(p[52:54]),svc_up=p[54],服务大小=p[55],服务arena=hex(q(56)),
   VDI原始=[hex(v) for v in p[58:82]],VDI基址=hex(q(80)),VDI引用=p[60],VDI_arena=hex(q(68)),
   客户映射=[hex(v) for v in p[82:87]],实例指针=[hex(q(88)),hex(q(90))],
   实例槽=[dict(占用=p[i],编号=p[i+1],核=p[i+2],状态=p[i+3],上下文=hex(q(i+4))) for i in range(92,116,6)],adapter状态=p[116],模式=p[117]))
 if len({r['会话'] for r in rows})>1:errors.append('多会话混合：不可当作一次连续记录')
 return rows,errors

def render(rows,errors):
 for e in errors:print('警告：'+e)
 print('|序号|事件|返回值|fin_rc / 失败阶段|worker|槽占用|核忙|svc_up|VDI|丢弃|')
 print('|---|---|---|---|---|---|---|---|---|---|')
 for r in rows:
  print('|'+ '|'.join(map(str,[r['序号'],r['事件'],r['返回值'],r['fin_rc']+' / '+r['失败阶段说明'],r['workers'],[s[0] for s in r['服务槽']],r['core忙'],r['svc_up'],r['VDI基址'],r['丢弃掩码']]))+'|')
 for r in rows:
  print(f"\n记录 {r['序号']}：会话={r['会话']}，计数器={r['计数器']}，Live有效={r['Live有效']}")
  for keys in [('close_failed','close_lock','Live流','Live忙','release_req','closes','eos_ok','preempts','历史阶段掩码'),('请求arena','请求大小','Live_arena','服务arena','服务大小','VDI_arena','VDI引用'),('客户映射','实例指针','实例槽'),('原始x0','原始x19','原始LR','准备IO错误','准备写入字节','准备close','卡代次','当前runtime_epoch','shutdown','arm','原因','hand_back已返回','诊断事务详情','事务阶段快照','adapter状态','模式')]:
   print('；'.join(f'{k}={r[k]}' for k in keys))
 print('\n有效扇区仅证明字节可读；最后一条不证明该次 CLOSE 返回。事件15只证明hand-back调用前已到达，不能证明其事务CLOSE返回；事件1证明hand-back已返回。只有15而无1仍无法区分hand-back卡住与返回后记录I/O卡住。事件13证明发送调用已返回，不证明MOVRS醒来、任务退出或录制成功。阶段16只证明先前事务调用返回，非介质持久化。')
if __name__=='__main__':
 a=argparse.ArgumentParser(description=__doc__);a.add_argument('file',type=Path);a.add_argument('--json',action='store_true');args=a.parse_args()
 rows,errors=decode(args.file.read_bytes())
 if args.json:print(json.dumps(dict(记录=rows,警告=errors),ensure_ascii=False,indent=2))
 else:render(rows,errors)
