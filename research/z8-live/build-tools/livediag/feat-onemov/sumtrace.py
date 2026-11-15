import json,collections,sys
seen=collections.Counter()
for l in open(sys.argv[1]):
    try:d=json.loads(l)
    except:continue
    pc=d['pc'];r=d['regs']
    if pc=='402dd4f8':
        s=bytes.fromhex(d.get('x0_mem','')).split(b'\0')[0]
        if b'DCIM' in s or b'LIVE' in s: print('open',s,r[4])
        continue
    k=(pc,r[4]);seen[k]+=1
    if seen[k]<=2 and pc not in('40cb7718','40cb7b54','40c270d0','40cb84cc','40344b20'):
        print(pc,r[:5], ('fi '+d['file_info'][:120]) if pc in('403ad6a4','403f7244') and 'file_info' in d else '')
print(seen.most_common(12))
