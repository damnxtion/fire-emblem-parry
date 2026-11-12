import sys,struct,subprocess,re
base,built,elf,out=sys.argv[1:5]
diff=int(sys.argv[5]) if len(sys.argv)>5 else None
b=bytearray(open(base,'rb').read()); g=open(built,'rb').read()
nm=subprocess.run(['arm-none-eabi-nm',elf],capture_output=True,text=True).stdout
sym={l.split()[2]:int(l.split()[0],16) for l in nm.splitlines() if len(l.split())==3}
hs=0xF00000
he=max(v for k,v in sym.items() if 0x08F00000<=v<0x09000000)-0x8000000+0x200
he=min(he,len(g))
while he>hs and g[he-1]==0: he-=1
he=(he+3)&~3
assert all(x in (0,0xFF) for x in b[hs:he]) or b[hs:he]==g[hs:he], 'hack region not free'
b[hs:he]=g[hs:he]
if diff is not None: b[hs+4]=diff
def hook(src,dst):
    o=sym[src]-0x8000000
    orig=g[o:o+8]; assert b[o:o+8]==orig or b[o:o+2]==b'\x00\x4b' or b[o:o+2]==b'\x01\x4b', src+' mismatch'
    t=sym[dst]|1
    if o%4==0: code=struct.pack('<HHI',0x4B00,0x4718,t)
    else: code=struct.pack('<HHHI',0x4B01,0x4718,0x46C0,t)
    b[o:o+len(code)]=code
    print('hook',src,hex(sym[src]),'->',hex(t))
hook('BattleGenerateHit','ParryBattleGenerateHit')
hook('DoCombatAction','ParryDoCombatAction')
hook('AiStartCombatAction','ParryAiStartCombatAction')
hook('BeginBattleAnimations','ParryBeginBattleAnimations')
hook('EkrBattleTriggerNewRoundStart','ParryEkrTriggerNewRoundStart')
hook('EkrBattleInRound','ParryEkrBattleInRound')
# fix header checksum
c=0
for i in range(0xA0,0xBD): c=(c-b[i])&0xFF
b[0xBD]=(c-0x19)&0xFF
open(out,'wb').write(b); print('hack bytes',hex(hs),hex(he))
