"""Explicit opt-in bounded console benchmark. Never called by builds or releases."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import socket
import statistics
import time
import urllib.error
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser()
parser.add_argument('--host', required=True)
parser.add_argument('--confirm-console-execution', action='store_true')
parser.add_argument('--extended', action='store_true', help='Include owner-approved 8/16 connections and 1 MiB TCP buffers.')
parser.add_argument('--follow-up', action='store_true', help='Start with the larger-buffer and 8/16-connection follow-up comparisons.')
parser.add_argument('--attempt',type=int,default=1,help='New recorded session only after the previous diagnostic stopped and was removed.')
parser.add_argument('--long-confirm',action='store_true',help='60-second / 512 MiB comparisons of 4/8/16 with larger TCP buffers.')
parser.add_argument('--skip-viking',action='store_true',help='Skip a provider whose saved test link failed file validation.')
args = parser.parse_args()
if not args.confirm_console_execution:
    parser.error('Explicit owner authorization is required before console execution.')
secret_header = (ROOT/'build/generated/benchmark-secret.h').read_text()
secret = re.search(r'ORBIT_BENCHMARK_SECRET "([a-f0-9]+)"', secret_header)[1]
run_id = re.search(r'ORBIT_BENCHMARK_ID "([a-f0-9]+)"', secret_header)[1]
token = json.loads((ROOT/'.state/console-tests/startup-test-auth.json').read_text())['token']
assert args.attempt>=1
record = ROOT/f'.state/console-tests/transfer-benchmark-{run_id}{"" if args.attempt==1 else "-attempt"+str(args.attempt)}.json'
assert not record.exists(), 'This build already had an attempt. Inspect its record; do not launch twice.'
if args.attempt>1:
    prior=ROOT/f'.state/console-tests/transfer-benchmark-{run_id}{"" if args.attempt==2 else "-attempt"+str(args.attempt-1)}.json'
    previous=json.loads(prior.read_text())
    assert previous.get('benchmarkStopped') and previous.get('temporaryElfRemoved'), 'Prior test cleanup is not confirmed.'
payload = (ROOT/'build/orbit_transfer_benchmark.elf').read_bytes()
assert payload.startswith(b'\x7fELF')
filename = f'orbit_speedtest_{run_id}.elf'
target = None  # Payload Manager chooses the subfolder; discover the exact uploaded path.
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

def request(port, path, data=None, timeout=8):
    headers = {}
    if port in (34177, 34179):
        headers['Authorization'] = 'Bearer '+(token if port==34177 else secret)
    if isinstance(data, dict):
        data=json.dumps(data).encode(); headers['Content-Type']='application/json'
    elif data is not None: headers['Content-Type']='application/octet-stream'
    req=urllib.request.Request(f'http://{args.host}:{port}{path}',data=data,headers=headers)
    with opener.open(req, timeout=timeout) as response: return response.read(4*1024*1024)

def get(port,path): return json.loads(request(port,path))
def queue():
    return [{k:j.get(k) for k in ('id','status','received','total','releaseId','storageId')}
            for j in get(34177,'/api/v1/downloads')]
def idle():
    assert all(j['status'] not in ('downloading','verifying','queued','retrying') for j in queue()), 'Orbit is busy.'
    assert not get(34177,'/api/v1/browser').get('active'), 'Provider browser is active.'
def daemons():
    return [{'pid':p['pid'],'name':p['name']} for p in get(8084,'/processes_list')['processes'] if p.get('is_daemon')]
def absent():
    try:
        with socket.create_connection((args.host,34179),timeout=2): return False
    except ConnectionRefusedError: return True

idle()
assert absent(), 'Benchmark port is already in use; nothing uploaded.'
before=queue(); processes=daemons(); paths=get(8084,'/list_payloads')['payloads']
assert not any(p.rsplit('/',1)[-1]==filename for p in paths)
out={'id':run_id,'sha256':hashlib.sha256(payload).hexdigest(),'beforeQueue':before,
     'beforeDaemons':processes,'payloadPath':target,'launchAttempts':0,'samples':[]}
def save():
    record.write_text(json.dumps(out,indent=2)+'\n'); record.chmod(0o600)
save()
uploaded=False; launched=False; benchmark_pid=None

def trial(label,connections,sock=0,buf=0,release='ppsa32557-ffpfsc',retry=True):
    idle()
    assert all(p in daemons() for p in processes), 'An existing daemon changed; stop testing.'
    body={'releaseId':release,'connections':connections,'socketBuffer':sock,'curlBuffer':buf,
          'bytes':(512 if args.long_confirm else 128)*1024*1024,'seconds':60 if args.long_confirm else 20}
    old=len(get(34179,'/status')['results'])
    request(34179,'/run',body)
    deadline=time.monotonic()+(160 if args.long_confirm else 95)
    next_memory=time.monotonic()+3; peak_memory=0
    while time.monotonic()<deadline:
        state=get(34179,'/status')
        if not state['busy'] and len(state['results'])>old: break
        if time.monotonic()>=next_memory and benchmark_pid is not None:
            matches=[p for p in get(8084,'/processes_list')['processes'] if p['pid']==benchmark_pid]
            if matches: peak_memory=max(peak_memory,float(matches[0].get('memory',0)))
            next_memory=time.monotonic()+5
        time.sleep(1)
    else: raise RuntimeError('Benchmark exceeded its response deadline.')
    result=state['results'][-1]
    points=result.get('speedSamples',[])
    if points and points[-1]['seconds']>0:
        end=points[-1]['seconds'];start=max(0,end-10)
        before=next((p for p in reversed(points) if p['seconds']<=start),points[0])
        after=next((p for p in points if p['seconds']>=start),points[-1])
        fraction=(start-before['seconds'])/(after['seconds']-before['seconds']) if after['seconds']>before['seconds'] else 0
        start_bytes=before['bytes']+fraction*(after['bytes']-before['bytes'])
        result['last10sNetworkMBps']=(points[-1]['bytes']-start_bytes)/(end-start)/1e6
    result['maxObservedMemoryMB']=peak_memory
    result['validSample']=bool(result['bytes'] and result.get('ranges') and
        all(r['validated'] for r in result['ranges']) and
        result.get('writerSucceeded',True) and
        (result['curlCode']==0 or result.get('timeLimitReached')))
    result['label']=label; out['samples'].append(result);save()
    print(json.dumps({'sample':len(out['samples']),'label':label,'MBps':round(result.get('MBps',0),3),
        'bytes':result['bytes'],'socketBuffers':[r['socketBufferAfter'] for r in result.get('ranges',[])],
        'bufferWaitSeconds':round(result.get('bufferWaitSeconds',0),3),'error':result['error']},separators=(',',':')),flush=True)
    assert result.get('temporaryFileRemoved'), 'Temporary-file cleanup was not confirmed.'
    if retry and not result['validSample'] and result.get('preflightStatus') in (500,502,504):
        delay=max(60,result.get('retryAt',0)-time.time())
        assert delay<=120, 'Provider asks for a longer wait; end this bounded test session.'
        print(f'HTTP {result["preflightStatus"]}; cooling down {delay:.0f}s before one retry.',flush=True)
        time.sleep(delay)
        return trial(label,connections,sock,buf,release,retry=False)
    assert result['validSample'], 'File validation or transfer failed; no further requests.'
    time.sleep(2)
    return result

try:
    uploaded=True; out['uploadAttempted']=True;save()
    response=request(8084,'/manage:upload?filename='+filename,payload,timeout=40).strip()
    assert response==b'OK', 'Payload Manager did not confirm the upload.'
    current=get(8084,'/list_payloads')['payloads']
    added=set(current)-set(paths)
    assert len(added)==1 and next(iter(added)).rsplit('/',1)[-1]==filename, 'Unexpected upload inventory.'
    target=next(iter(added)); out['payloadPath']=target;save()
    out['launchAttempts']=1;save();launched=True
    try: request(8084,'/loadpayload:'+urllib.parse.quote(target,safe='/'),timeout=12)
    except urllib.error.URLError: pass  # Do not retry an uncertain launch.
    for _ in range(20):
        try:
            state=get(34179,'/status')
            if state.get('name')=='Orbit isolated transfer benchmark': break
        except urllib.error.URLError: pass
        time.sleep(1)
    else: raise RuntimeError('No benchmark startup confirmation; not launching again.')
    added_processes=[p for p in daemons() if p not in processes]
    if len(added_processes)==1: benchmark_pid=added_processes[0]['pid']
    out['benchmarkPid']=benchmark_pid;save()
    print('Isolated benchmark running; existing Orbit and other payloads remain active.',flush=True)
    configs=([('4-tcp1m',4,1048576,0),('8-tcp1m',8,1048576,0),('16-tcp1m',16,1048576,0)] if args.long_confirm else
             [('4-tcp1m',4,1048576,0),('8-tcp256k',8,262144,0),('16-tcp256k',16,262144,0),
              ('16-default',16,0,0),('8-default',8,0,0),('4-default',4,0,0),('4-tcp256k',4,262144,0)] if args.follow_up else
             [('4-default',4,0,0),('4-tcp256k',4,262144,0),('8-default',8,0,0),
              ('8-tcp256k',8,262144,0),('16-default',16,0,0),('16-tcp256k',16,262144,0),
              ('4-tcp1m',4,1048576,0)] if args.extended else
             [('2-default',2,0,0),('4-default',4,0,0),('4-tcp256k',4,262144,0),('4-tcp256k-curl256k',4,262144,262144)])
    for config in configs+list(reversed(configs)): trial(*config)
    def median(label):
        return statistics.median(r['MBps'] for r in out['samples'] if r['label']==label and r.get('validSample'))
    eight=[]
    if not args.extended and not args.follow_up and not args.long_confirm:
        best4=max(configs[1:],key=lambda c:median(c[0]))
        eight=[('8-default',8,0,0),('8-best-buffers',8,best4[2],best4[3])]
        for config in eight+list(reversed(eight)): trial(*config)
    winner=max(configs+eight,key=lambda c:median(c[0]))
    out['archiveMedianMBps']={c[0]:median(c[0]) for c in configs+eight}
    out['archiveWinner']=winner;save()
    viking=[] if args.skip_viking else [('viking-4-default',4,0,0),('viking-archive-winner',*winner[1:])]
    for config in viking+list(reversed(viking)):
        trial(*config,release='ppsa04930-viking-exfat')
except Exception as e:
    out['error']=type(e).__name__+': '+str(e);save();print('Benchmark stopped:',out['error'],flush=True)
finally:
    if launched:
        try:
            request(34179,'/stop',{})
            for _ in range(35):
                if absent(): break
                time.sleep(1)
            out['benchmarkStopped']=absent()
        except Exception as e: out['cleanupError']=type(e).__name__
    if uploaded:
        current=get(8084,'/list_payloads')['payloads']
        owned=[p for p in current if p.rsplit('/',1)[-1]==filename and p not in paths]
        if len(owned)==1:
            request(8084,'/manage:delete?filename='+urllib.parse.quote(filename,safe=''))
        out['temporaryElfRemoved']=not any(p.rsplit('/',1)[-1]==filename for p in get(8084,'/list_payloads')['payloads'])
    out['afterQueue']=queue();out['queuePreserved']=out['afterQueue']==before
    after=daemons();out['otherDaemonsPreserved']=all(p in after for p in processes)
    out['afterDaemons']=after;save()
    print(json.dumps({k:out.get(k) for k in ('archiveMedianMBps','archiveWinner','benchmarkStopped',
          'temporaryElfRemoved','queuePreserved','otherDaemonsPreserved','error')},indent=2),flush=True)
