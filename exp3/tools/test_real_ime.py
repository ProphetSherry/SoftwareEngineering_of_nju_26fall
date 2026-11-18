# Real X11 keyboard -> Fcitx5 -> Qt -> SQLite integration check.
import ctypes as C
import os
import subprocess
import time
import dbus
from pathlib import Path

x = C.CDLL('libX11.so.6')
t = C.CDLL('libXtst.so.6')
x.XOpenDisplay.argtypes=[C.c_char_p]; x.XOpenDisplay.restype=C.c_void_p
x.XSetInputFocus.argtypes=[C.c_void_p,C.c_ulong,C.c_int,C.c_ulong]
x.XFlush.argtypes=[C.c_void_p]
x.XStringToKeysym.argtypes=[C.c_char_p]; x.XStringToKeysym.restype=C.c_ulong
x.XKeysymToKeycode.argtypes=[C.c_void_p,C.c_ulong]; x.XKeysymToKeycode.restype=C.c_ubyte
for fn,args in [('XTestFakeMotionEvent',[C.c_void_p,C.c_int,C.c_int,C.c_int,C.c_ulong]),('XTestFakeButtonEvent',[C.c_void_p,C.c_uint,C.c_int,C.c_ulong]),('XTestFakeKeyEvent',[C.c_void_p,C.c_uint,C.c_int,C.c_ulong])]:
 getattr(t,fn).argtypes=args
screen=x.XOpenDisplay(None)
if not screen: raise SystemExit('Cannot connect to actual X11 display')
env=os.environ.copy(); env['QT_QPA_PLATFORM']='xcb'; env['CAMPUS_REAL_IME_TEST']='1'
log=open('/tmp/campus-real-ime-stderr.log','w')
project=Path(__file__).resolve().parent.parent
app=subprocess.Popen([str(project/'build/campus_gui_tests')],env=env,stdout=subprocess.PIPE,stderr=log,text=True)
try:
 line=app.stdout.readline().strip()
 print(line,flush=True)
 if not line.startswith('READY '): raise RuntimeError('GUI test did not become ready')
 fields=list(map(int,line.split()[1:])); window=fields[0]
 controller=dbus.Interface(dbus.SessionBus().get_object('org.fcitx.Fcitx5','/controller'),'org.fcitx.Fcitx.Controller1')
 for i,word in enumerate(['nihao','zhongwen','ceshi']):
  x.XSetInputFocus(screen,window,2,0)
  t.XTestFakeMotionEvent(screen,-1,fields[1+i*2],fields[2+i*2],0)
  t.XTestFakeButtonEvent(screen,1,1,0); t.XTestFakeButtonEvent(screen,1,0,0); x.XFlush(screen)
  time.sleep(.3)
  controller.SetCurrentIM('pinyin')
  controller.Activate()
  print('Typing',word,'with engine',str(controller.CurrentInputMethod()),flush=True)
  time.sleep(.2)
  for char in word+' ':
   symbol=x.XStringToKeysym(b'space' if char==' ' else char.encode())
   code=x.XKeysymToKeycode(screen,symbol)
   t.XTestFakeKeyEvent(screen,code,1,0);t.XTestFakeKeyEvent(screen,code,0,0);x.XFlush(screen)
   time.sleep(.1)
  time.sleep(.5)
 out=app.communicate(timeout=35)[0]
 print(out,flush=True)
 print('Exit:',app.returncode,flush=True)
 if app.returncode: raise RuntimeError('Real input test failed; see /tmp/campus-real-ime-stderr.log')
finally:
 if app.poll() is None: app.terminate(); app.wait(timeout=5)
 log.close()
