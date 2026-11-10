"""600x1024 layout previews, NOT LVGL/P4 screenshots.

Uses the firmware's home table, palette, fixed A4 masks, ui_layout.h metrics
and source font. Other pages model their control geometry; native widgets,
SD font bearings, touch and live business states still require device review.
Synthetic text never reaches chat storage.
"""
from pathlib import Path
import json
import re
from PIL import Image, ImageDraw, ImageFont

ROOT=Path(__file__).resolve().parents[1]
MAIN=ROOT/'firmware/p4_main/main'
OUT=ROOT/'assets/qinghe-review'
OUT.mkdir(exist_ok=True)
FONT=ROOT/'firmware/p4_main/managed_components/lvgl__lvgl/scripts/generators/built_in_font/SourceHanSansSC-Normal.otf'
MACROS={k:int(v,0) for k,v in re.findall(r'#define (QH_\w+|UI_\w+) (0x[0-9A-F]+|\d+)\b',
    (MAIN/'qinghe_ui.h').read_text()+(MAIN/'ui_layout.h').read_text())}
PALETTE=[]
for a,b in re.findall(r'\{(QH_\w+|0x[0-9A-F]+), (0x[0-9A-F]+)\}',(MAIN/'qinghe_theme.c').read_text()):
    PALETTE.append((MACROS[a] if a in MACROS else int(a,16),int(b,16)))
HOME=json.loads((ROOT/'assets/qinghe-home/layout.json').read_text(encoding='utf-8'))


class Preview:
    def __init__(self,dark,size):
        self.dark=dark;self.size=size;self.lh=27 if size==14 else 54
        self.im=Image.new('RGB',(600,1024),self.color('QH_BG'));self.d=ImageDraw.Draw(self.im)
    def color(self,role):
        n=MACROS.get(role,int(role,16) if role[0]!='Q' else 0)
        if self.dark:
            n=next((b for a,b in PALETTE if a==n),n)
        return '#'+f'{n:06X}'
    def rect(self,box,color='QH_SURFACE',radius=20,border=None):
        x,y,w,h=box
        self.d.rounded_rectangle((x,y,x+w-1,y+h-1),radius=radius,fill=self.color(color),
                                 outline=self.color(border) if border else None,width=1)
    def text(self,text,box,color='QH_INK',size=None,align='left',wrap=False):
        x,y,w,h=box;size=size or self.size
        font=ImageFont.truetype(str(FONT),size);lh=28 if size==24 else 54 if size==28 else 27
        lines=[]
        for original in text.split('\n'):
            line=''
            for c in original:
                if wrap and line and self.d.textlength(line+c,font=font)>w:
                    lines.append(line);line=''
                line+=c
            lines.append(line)
        top=y+max(0,(h-lh*len(lines))//2)
        for i,line in enumerate(lines):
            width=self.d.textlength(line,font=font)
            tx=x+(w-width)/2 if align=='center' else x+w-width if align=='right' else x
            self.d.text((round(tx),top+i*lh+(lh-size)//2),line,font=font,fill=self.color(color),anchor='lt')
        return len(lines)*lh
    def icon(self,kind,x,y):
        ink=self.color('QH_INK');d=self.d
        if kind in ['settings','camera','document','keyboard','mic','arrow']:
            im=Image.open(ROOT/f'assets/qinghe-home/{kind}.png').convert('RGBA')
            self.im.paste(ink,(x+4,y+4,x+28,y+28),im.getchannel('A'));return
        if kind=='back':d.line([(x+20,y+6),(x+10,y+16),(x+20,y+26)],fill=ink,width=2)
        elif kind=='robot':
            d.rounded_rectangle((x+4,y+10,x+28,y+28),radius=5,outline=ink,width=2)
            d.line((x+16,y+3,x+16,y+10),fill=ink,width=2)
            for dx in [10,20]:d.ellipse((x+dx,y+16,x+dx+3,y+19),fill=ink)
            d.line((x+12,y+24,x+21,y+24),fill=ink,width=2)
        elif kind=='user':
            d.ellipse((x+11,y+3,x+21,y+13),outline=ink,width=2)
            d.rounded_rectangle((x+5,y+19,x+27,y+29),radius=6,outline=ink,width=2)
        else:
            d.ellipse((x+7,y+7,x+25,y+25),outline=ink,width=2)
            if not self.dark:d.ellipse((x+15,y+3,x+30,y+19),fill=self.color('QH_SOFT'))
            else:
                for line in [(16,1,16,4),(16,28,16,31),(1,16,4,16),(28,16,31,16),
                             (5,5,7,7),(25,25,27,27),(5,27,7,25),(25,7,27,5)]:
                    d.line(tuple(v+(x if i%2==0 else y) for i,v in enumerate(line)),fill=ink,width=2)
    def header(self,title,ime=False,settings=True):
        self.rect((20,12,56,56),'QH_SOFT');self.icon('back',32,24)
        title_size=28 if title in ['语音对话','独立转写'] else self.size
        self.text(title,(92,12,284,56),size=title_size)
        for x,kind in [(456,'theme')]+([(524,'settings')] if settings else []):
            self.rect((x,12,56,56),'QH_SOFT');self.icon(kind,x+12,24)
        if ime:self.rect((388,12,56,56),'QH_SOFT');self.text('拼',(388,12,56,56),align='center')
    def input(self):
        self.rect((20,940,52,52),'QH_SOFT');self.icon('keyboard',30,950)
        self.rect((80,940,432,64),'QH_SURFACE',18,'QH_LINE')
        self.text('输入文字',(92,940,400,64),'QH_MUTED',size=28)
        self.rect((520,944,60,52),'QH_ACCENT');self.text('↑',(520,944,60,52),'QH_ON_ACCENT',align='center')
    def save(self,name):
        self.d.text((8,1010),'LAYOUT PREVIEW / NOT A DEVICE SCREENSHOT',fill=self.color('QH_MUTED'))
        path=OUT/f'{name}-{self.size}-{"dark" if self.dark else "light"}.png';self.im.save(path);return path


def home(p):
    for v in HOME['items']:
        box=v['box28'] if p.size==28 else v['box'];x,y,w,h=box;k=v['kind']
        if k in ['button','panel']:p.rect(box,v['color'],v.get('radius',0),v.get('border'))
        elif k=='label':
            size=28 if v.get('font')=='heading' else 14 if v.get('font')=='latin' else p.size
            p.text(v['text'],box,v['color'],size,v.get('align','left'))
        elif k=='image':
            res=v['resource']
            im=Image.open(ROOT/('assets/brand/native-icon-64.png' if res=='native' else f'assets/qinghe-home/{res}.png')).convert('RGBA')
            if res=='native':p.im.paste(im,(x,y),im)
            else:p.im.paste(p.color(v['color']),(x,y,x+w,y+h),im.getchannel('A'))
        elif k=='wave':
            for i,(wh,c) in enumerate(zip(HOME['wave_heights'],HOME['wave_colors'])):
                p.rect((x+i*20,y+(h-wh)//2,10,wh),c,5)
        assert 0<=x and 0<=y and x+w<=600 and y+h<=1024,v['id']
    y=58 if p.size==28 else 48;p.rect((456,y,56,56),'QH_SOFT');p.icon('theme',468,y+12)


def chat(p):
    p.header('语音对话',True)
    p.text('声音与文字，按你的方式',(20,82,560,54),'QH_MUTED')
    p.text('按住说话，松开发送',(20,136,560,72),align='center')
    y=MACROS['UI_CHAT_TOP']
    for role,text in [('robot','你好，我们可以说话，也可以打字。'),('user','帮我把这句话表达得更清楚。'),
                      ('robot','慢慢说，我在听。长句会自动换行；阅读旧消息时，保留你当前的位置。')]:
        line=p.lh;chars=27 if p.size==14 else 14
        count=(len(text)+chars-1)//chars;h=count*line+24
        if y+h>MACROS['UI_CHAT_TOP']+MACROS['UI_CHAT_HEIGHT']:break
        user=role=='user';bx=104 if user else 84
        p.rect((bx,y,400,h),'QH_USER' if user else 'QH_ASSISTANT',24,'QH_LINE')
        p.text(text,(bx+12,y+12,376,h-24),wrap=True)
        ax=512 if user else 32;p.rect((ax,y,44,44),'QH_USER' if user else 'QH_SOFT',12,'QH_LINE');p.icon(role,ax+6,y+6)
        y+=h+8
    p.rect((20,840,560,70),'QH_ACCENT');p.text('按住说话，松开发送',(20,840,560,70),'QH_ON_ACCENT',align='center');p.input()


def settings(p):
    p.header('设置',settings=False);y=MACROS['UI_SETTINGS_TOP'];hit=max(56,p.lh+12)
    names=['Wi-Fi','屏幕亮度','扬声器音量','全局字号','对话历史','自动息屏','关于设备','使用帮助']
    # Same row metrics as settings_relayout, not equal-height flex stretching.
    for i,name in enumerate(names):
        if i in [0,1,3,5]:
            p.text({0:'连接',1:'屏幕与声音',3:'文字与会话',5:'设备'}[i],(32,y,530,p.lh),'QH_MUTED');y+=p.lh+8
        extra=MACROS['UI_SETTINGS_SLIDER_EXTRA'] if i in [1,2] else MACROS['UI_SETTINGS_CHOICE_EXTRA'] if i==3 else MACROS['UI_SETTINGS_HISTORY_EXTRA'] if i==4 else 0
        h=p.lh+extra+(hit if i==3 else 0) if extra else hit+16
        p.rect((26,y,548,h),'QH_SURFACE',20,'QH_LINE');p.text(name,(46,y+10,246,p.lh))
        if i in [1,2]:
            p.text('55%' if i==1 else '59%',(370,y+10,176,p.lh),'QH_MUTED',align='right')
            sy=y+h-26-18;p.rect((80,sy,440,18),'QH_LINE',9);p.rect((80,sy,250,18),'QH_ACCENT',9)
            p.rect((316,sy-10,38,38),'QH_INK',19)
        elif i==3:
            for j,label in enumerate(['14px','28px']):
                x=100+j*220;p.rect((x,y+h-16-hit,180,hit),'QH_SOFT' if p.size==int(label[:2]) else 'QH_SURFACE',24,'QH_LINE')
                p.text(label,(x,y+h-16-hit,180,hit),align='center')
        elif i==4:
            p.text('已有记录保留',(46,y+h-p.lh-16,240,p.lh),'QH_MUTED')
            p.rect((366,y+h-16-hit,160,hit),'QH_DANGER');p.text('清除历史',(366,y+h-16-hit,160,hit),'QH_ON_ACCENT',align='center')
        else:
            label={0:'管理网络',5:'30秒',6:'查看信息',7:'查看帮助'}[i]
            p.rect((350,y+8,196,hit),'QH_SOFT');p.text(label,(350,y+8,196,hit),align='center')
        y+=h+8
    p.rect((20,MACROS['UI_SETTINGS_TOP']+MACROS['UI_SETTINGS_HEIGHT'],560,1024-MACROS['UI_SETTINGS_TOP']-MACROS['UI_SETTINGS_HEIGHT']),'QH_BG',0)
    p.text('调节后自动保存',(20,968,560,p.lh),'QH_MUTED',align='center')


def startup(p):
    bits=bytes([0x10,0x70,0x58,0xbb,0x6e,0x0d,0x02,0x04,0x3e,0x10,0x00])
    for row in range(9):
        for col in range(9):
            leaf=(bits[(row*9+col)//8]>>((row*9+col)%8))&1;r=6 if leaf else 1
            x=192+12+col*24;y=208+12+row*24;dist=abs(row-4)
            c='QH_LINE' if not leaf else 'QH_ACCENT' if dist==0 else 'QH_MUTED' if dist==1 else 'QH_LINE'
            p.rect((x-r,y-r,r*2+1,r*2+1),c,r)
    p.text('灵译通',(40,466,520,28),size=24,align='center')
    p.text('每一种表达，都被听见。',(40,524,520,28),'QH_MUTED',size=24,align='center')
    p.text('资源准备中，可独立完成',(40,748,520,84),'QH_MUTED',size=24,align='center')
    for i,(name,state,c) in enumerate([('字库','完成','QH_ACCENT'),('输入法','准备中','QH_MUTED'),('联网','完成','QH_ACCENT')]):
        x=44+i*176;p.text(name+'\n'+state,(x,856,160,64),size=24,align='center');p.rect((x,930,160,4),c,2)


def overlays(p):
    p.header('独立转写',True)
    p.text('候选与确认框布局样例',(20,86,560,54),'QH_MUTED',align='center')
    p.rect((20,280,560,336),'QH_SOFT')
    p.text('nihao',(32,288,530,64))
    for j,t in enumerate(['你好','您好','拟好']):
        p.rect((32+j*174,370,164,64),'QH_SURFACE');p.text(t,(32+j*174,370,164,64),align='center')
    p.text('1/2 · 可横向滑动',(32,550,530,54),'QH_MUTED')
    for row,letters in enumerate(['Q W E R T Y U I O P','A S D F G H J K L','Z X C V B N M']):
        p.rect((20,704+row*68,560,60),'QH_SURFACE',12);p.text(letters,(30,704+row*68,540,60),size=14,align='center')
    p.rect((40,150,520,250),'QH_SURFACE',28,'QH_LINE')
    p.text('清空全部转写文字？',(60,166,480,116),align='center',wrap=True)
    for i,t in enumerate(['取消','确认清空']):
        p.rect((60+i*250,310,230,70),'QH_SOFT' if i==0 else 'QH_DANGER')
        p.text(t,(60+i*250,310,230,70),'QH_INK' if i==0 else 'QH_ON_ACCENT',align='center')


paths=[]
for size in [14,28]:
    for dark in [False,True]:
        for name,draw in [('home',home),('chat',chat),('settings',settings),('startup',startup),('overlays',overlays)]:
            p=Preview(dark,size);draw(p);paths.append(p.save(name))
    sheet=Image.new('RGB',(1200,1060),'#D6DCD3');d=ImageDraw.Draw(sheet)
    for row,dark in enumerate([False,True]):
        for col,name in enumerate(['home','chat','settings','startup']):
            p=OUT/f'{name}-{size}-{"dark" if dark else "light"}.png'
            sheet.paste(Image.open(p).resize((300,512)),(col*300,row*530+18))
            d.text((col*300+8,row*530+3),f'{name} / {size}px / {"dark" if dark else "light"} / LAYOUT',fill='#203327')
    sheet.save(OUT/f'review-{size}.png')

# All top controls retain a 56px hit area and 12px gaps, even with IME.
tools=[(20,56),(388,56),(456,56),(524,56)]
assert all(x>=0 and x+w<=600 and w>=48 for x,w in tools)
assert all(b-(a+w)>=12 for (a,w),(b,_) in zip(tools,tools[1:]))
(OUT/'README.md').write_text('''# 青禾亮暗布局预览

这些600×1024图是**布局预览，不是LVGL帧缓冲或实屏截图**。首页读取固件同一布局表和量化A4资源；色值读取固件语义调色板，其他页面按固件控件几何和真实27/54行高绘制。原生键盘、字体细节、滚动和触控仍需实机验收。消息为离线合成，不写入设备或聊天记录。overlays展示颜色/可读性，不代表真实确认框与键盘同时显示。
''',encoding='utf-8')
print(json.dumps({'preview_count':len(paths),'canvas':[600,1024],'kind':'layout preview, not LVGL render','toolbar_geometry':'PASS'},ensure_ascii=False))
