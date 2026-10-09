#!/usr/bin/env python3
"""Display-only V-Webcam for R2's existing Eyes stream."""
import fcntl, json, os, tkinter as tk
from pathlib import Path
from tkinter import ttk
DATA=Path("/tmp/r2-vwebcam-r2"); FRAME=DATA/"frame.ppm"; STATUS=DATA/"status.txt"; FOCUS=DATA/"focus.json"; LOCK=DATA/"viewer.lock"
GREEN="#42f58d"; BG="#111820"; PANEL="#1a2530"; TEXT="#e8f0f7"; MUTED="#8fa3b7"
class VWebcam:
 def __init__(self,root):
  self.root=root; root.title("R2-3PO | V-Webcam"); root.configure(bg=BG); root.geometry("1050x700"); root.minsize(640,420)
  self.paused=False; self.show_focus=True; self.photo=None; self.last=None; self.w=self.h=0
  top=tk.Frame(root,bg=PANEL,padx=12,pady=9); top.pack(fill="x")
  tk.Label(top,text="◉  R2-3PO  /  V-WEBCAM",bg=PANEL,fg=TEXT,font=("DejaVu Sans",13,"bold")).pack(side="left")
  self.live=tk.Label(top,text="● WAITING FOR EYES",bg=PANEL,fg="#ffbd59",font=("DejaVu Sans",9,"bold")); self.live.pack(side="right")
  self.canvas=tk.Canvas(root,bg="#05080b",highlightthickness=0); self.canvas.pack(fill="both",expand=True,padx=10,pady=(10,4))
  self.image=self.canvas.create_image(0,0,anchor="nw"); self.box=self.canvas.create_rectangle(0,0,0,0,outline=GREEN,width=3,state="hidden")
  self.tag=self.canvas.create_text(8,8,anchor="nw",text="",fill=GREEN,font=("DejaVu Sans",10,"bold"),state="hidden")
  info=tk.Frame(root,bg=PANEL,padx=12,pady=8); info.pack(fill="x",padx=10,pady=(4,8))
  self.source=tk.Label(info,text="Source: waiting",bg=PANEL,fg=TEXT,font=("DejaVu Sans",10,"bold"),anchor="w"); self.source.pack(fill="x")
  self.details=tk.Label(info,text="No frame received yet.",bg=PANEL,fg=MUTED,font=("DejaVu Sans",9),anchor="w"); self.details.pack(fill="x",pady=(3,0))
  row=tk.Frame(info,bg=PANEL); row.pack(fill="x",pady=(8,0))
  self.pause_btn=ttk.Button(row,text="Pause display",command=self.toggle_pause); self.pause_btn.pack(side="left")
  self.focus_btn=ttk.Button(row,text="Hide focus box",command=self.toggle_focus); self.focus_btn.pack(side="left",padx=(6,0))
  ttk.Button(row,text="Refresh now",command=self.refresh).pack(side="left",padx=(6,0))
  tk.Label(row,text="Display only • no camera • no control of Eyes",bg=PANEL,fg=MUTED,font=("DejaVu Sans",8)).pack(side="right")
  root.bind("<space>",lambda _e:self.toggle_pause()); root.bind("f",lambda _e:self.toggle_focus()); root.bind("<Escape>",lambda _e:root.iconify())
  root.after(100,self.tick); self.refresh()
 def toggle_pause(self):
  self.paused=not self.paused; self.pause_btn.configure(text="Resume display" if self.paused else "Pause display")
  if not self.paused:self.refresh()
 def toggle_focus(self):
  self.show_focus=not self.show_focus; self.focus_btn.configure(text="Hide focus box" if self.show_focus else "Show focus box"); self.draw_focus()
 def status(self):
  try:
   r=STATUS.read_text(errors="replace").splitlines(); r+=[""]*max(0,6-len(r))
   return {"active":r[0].strip()=="1","source":r[1].strip() or "Unknown","width":int(r[2] or 0),"height":int(r[3] or 0),"frames":int(r[4] or 0)}
  except (OSError,ValueError):return {"active":False,"source":"No source","width":0,"height":0,"frames":0}
 def focus(self):
  try:
   o=json.loads(FOCUS.read_text()); v=[float(o[k]) for k in ("x","y","width","height")]
   if not all(0<=n<=1 for n in v) or v[2]<=0 or v[3]<=0 or v[0]+v[2]>1.001 or v[1]+v[3]>1.001:return None
   return {"x":v[0],"y":v[1],"width":v[2],"height":v[3],"label":str(o.get("label","Selected region"))}
  except (OSError,ValueError,KeyError,TypeError):return None
 def refresh(self):
  if not self.paused:
   try:
    stamp=FRAME.stat().st_mtime_ns
    if stamp!=self.last:
     self.photo=tk.PhotoImage(file=str(FRAME)); self.w,self.h=self.photo.width(),self.photo.height(); self.last=stamp
     self.canvas.itemconfigure(self.image,image=self.photo); self.canvas.tag_lower(self.image); self.canvas.coords(self.image,0,0)
   except (OSError,tk.TclError):pass
  self.draw_focus()
 def draw_focus(self):
  s,f=self.status(),self.focus(); self.live.configure(text="● EYES ACTIVE" if s["active"] else "● WAITING FOR EYES",fg=GREEN if s["active"] else "#ffbd59")
  self.source.configure(text=f"Source: {s['source']}")
  self.details.configure(text=(f"Native input: {s['width']}×{s['height']}  •  Frame: {s['frames']}  •  Display: {self.w}×{self.h}  •  Focus: {f['label'] if f else 'No focus region reported yet'}") if self.w else "Waiting for a frame from R2's existing Eyes subsystem.")
  if not self.show_focus or not f or not self.w or not self.h:
   self.canvas.itemconfigure(self.box,state="hidden"); self.canvas.itemconfigure(self.tag,state="hidden"); return
  x1,y1=f["x"]*self.w,f["y"]*self.h; x2,y2=(f["x"]+f["width"])*self.w,(f["y"]+f["height"])*self.h
  self.canvas.coords(self.box,x1,y1,x2,y2); self.canvas.itemconfigure(self.box,state="normal")
  self.canvas.coords(self.tag,max(4,x1),max(4,y1-18)); self.canvas.itemconfigure(self.tag,text=f["label"],state="normal")
 def tick(self):self.refresh(); self.root.after(100,self.tick)
def main():
 if not os.environ.get("DISPLAY"):return
 DATA.mkdir(mode=0o700,parents=True,exist_ok=True); lock=open(LOCK,"w")
 try:fcntl.flock(lock.fileno(),fcntl.LOCK_EX|fcntl.LOCK_NB)
 except BlockingIOError:return
 root=tk.Tk(); VWebcam(root); root.mainloop()
if __name__=="__main__":main()
