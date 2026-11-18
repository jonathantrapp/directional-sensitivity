"""Supplement REA with bounded, read-only Unity metadata/resource inspection.

This indexes strings and shipped configuration, not executable method bodies.
Every hit retains its original byte offset and artifact identity.
"""
import argparse
import hashlib
import json
import re
import struct
from collections import Counter
from pathlib import Path

PATTERN=re.compile(r'pure.?vector|corsair|directional|anisotrop|sensitiv|fortnite|overshoot|undershoot|underflick|overflick|aimbias|aim.bias|replay|flick.*(end|start|duration|phase)|cm.?360|mouse.*(delta|record)',re.I)
def inspect_metadata(path):
    data=path.read_bytes()
    if len(data)<32: raise ValueError('Truncated metadata')
    magic,version,literal_offset,literal_bytes,literal_data_offset,literal_data_bytes,names_offset,names_bytes=struct.unpack_from('<8I',data)
    if magic!=0xfab11baf or version not in (24,27,29,31): raise ValueError('Unsupported IL2CPP metadata header')
    for offset,size in ((literal_offset,literal_bytes),(literal_data_offset,literal_data_bytes),(names_offset,names_bytes)):
        if offset<32 or offset+size>len(data): raise ValueError('Metadata string table outside file')
    hits=[]; offset=names_offset; count=0
    for value in data[names_offset:names_offset+names_bytes].split(b'\0'):
        text=value.decode('utf-8',errors='replace'); count+=1
        if PATTERN.search(text): hits.append(dict(kind='name_pool',offset=offset,value=text))
        offset+=len(value)+1
    if literal_bytes%8: raise ValueError('Invalid literal descriptor table length')
    for entry in range(literal_offset,literal_offset+literal_bytes,8):
        length,index=struct.unpack_from('<II',data,entry)
        if index+length>literal_data_bytes: raise ValueError('Literal outside string data')
        if length>1000000: continue
        text=data[literal_data_offset+index:literal_data_offset+index+length].decode('utf-8',errors='replace')
        if PATTERN.search(text):
            hits.append(dict(kind='literal',offset=literal_data_offset+index,descriptor_offset=entry,value=text))
    types=[]
    if version==31:
        # Version-specific metadata declarations, checked against Il2CppDumper's
        # MetadataClass.cs. No code pointers or method bodies are inferred.
        def table(pair,stride):
            offset,size=struct.unpack_from('<II',data,8+pair*8)
            if size%stride or offset+size>len(data): raise ValueError('Invalid metadata declaration table')
            return offset,size//stride
        type_offset,type_count=table(19,88)
        method_offset,method_count=table(5,36)
        field_offset,field_count=table(11,12)
        def name(index):
            if index>=names_bytes: raise ValueError('Name outside metadata pool')
            end=data.find(b'\0',names_offset+index,names_offset+names_bytes)
            if end<0: raise ValueError('Unterminated declaration name')
            return data[names_offset+index:end].decode('utf-8')
        for i in range(type_count):
            at=type_offset+i*88
            ni,ns=struct.unpack_from('<II',data,at)
            type_name,namespace=name(ni),name(ns)
            if not (namespace.startswith('AimLab.OmniSense') or re.search('BallisticTiming|FlickDetection|FlickingError|SensitivityAnalyzer|SensitivityConverter|MouseSensitivity|SensitivityManager',type_name)):
                continue
            fs,ms=struct.unpack_from('<ii',data,at+32)
            mc,pc,fc=struct.unpack_from('<HHH',data,at+64)
            if (fc and (fs<0 or fs+fc>field_count)) or (mc and (ms<0 or ms+mc>method_count)):
                raise ValueError('Member range outside table')
            methods=[];fields=[]
            for j in range(ms,ms+mc):
                ma=method_offset+j*36
                mn,owner=struct.unpack_from('<Ii',data,ma)
                if owner!=i: raise ValueError('Method ownership mismatch: unsupported layout')
                methods.append(dict(name=name(mn),metadata_offset=ma,token=hex(struct.unpack_from('<I',data,ma+24)[0])))
            for j in range(fs,fs+fc):
                fa=field_offset+j*12
                fn,ft,token=struct.unpack_from('<IiI',data,fa)
                fields.append(dict(name=name(fn),metadata_offset=fa,type_index=ft,token=hex(token)))
            types.append(dict(name=type_name,namespace=namespace,metadata_offset=at,methods=methods,fields=fields))
    return dict(path=str(path),bytes=len(data),sha256=hashlib.sha256(data).hexdigest(),version=version,
        name_entries=count,literal_entries=literal_bytes//8,hits=hits,
        selected_types=types,
        limitations=['Names/literals are not method bodies, call relationships, or observed execution.'])

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('installation',type=Path); parser.add_argument('--output',required=True,type=Path)
    args=parser.parse_args()
    if args.output.exists(): parser.error('Output directory must be new')
    metadata=args.installation/'AimLab_tb_Data/il2cpp_data/Metadata/global-metadata.dat'
    report=inspect_metadata(metadata)
    assets=args.installation/'AimLab_tb_Data/StreamingAssets'
    configs=[]
    for p in sorted(assets.rglob('*.json')):
        if not re.search(r'Fortnite|Overflick|Underflick|_defaultProfile_|Sensitivity|Spider',p.name,re.I): continue
        if p.stat().st_size>2000000: continue
        data=p.read_bytes()
        try: parsed=json.loads(data)
        except (ValueError,UnicodeDecodeError): continue
        configs.append(dict(path=str(p),sha256=hashlib.sha256(data).hexdigest(),bytes=len(data),content=parsed))
    report['selected_shipped_configs']=configs
    args.output.mkdir(parents=True)
    (args.output/'metadata-evidence.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    for term in ('purevector','corsair','fortnite','directional','sensitivity','overshoot','underflick','replay'):
        matches=[h for h in report['hits'] if term in h['value'].lower()]
        print(f'{term}: {len(matches)} hits')
        for hit in matches[:16]: print(hex(hit['offset']),hit['kind'],hit['value'][:180].replace('\n',' '))
    print('Metadata version',report['version'],'name entries',report['name_entries'],'literal entries',report['literal_entries'])
    print('Selected config files:',len(configs))
    print('Selected type declarations:',len(types) if (types:=report['selected_types']) else 0)
    for t in types:
        if t['name'] in ('CorsairMouseHardwareAdapter','OmniSenseSensitivityProfile','BallisticTimingAnalyzer','FlickDetectionTuning','FlickingErrorMetric','RadialSensitivityAnalyzer','AxisSensitivityAnalyzer'):
            print(t['namespace']+'.'+t['name'], 'methods:',', '.join(m['name'] for m in t['methods']))

if __name__=='__main__': main()
