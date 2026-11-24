# AL-95: copied from gdd/ui/client_2d/mockups/src (the design source); gen_sprites_2d.py reads it.
# Спрайты 16x16: сетки символов -> SVG rects. Палитра общая.
import random
PAL = {
 'k':'#1a1410','s':'#e0a878','S':'#b0744c','h':'#5a3a20','H':'#d8b048','g':'#a8a8a0',
 'r':'#a82820','R':'#701810','b':'#3050a0','B':'#203070','G':'#4c8a34','D':'#2c5a20','K':'#1c3a18',
 'l':'#7ab848','y':'#e8c040','o':'#c08030','w':'#e8e0d0','W':'#8a8a88','m':'#c8ccd0','M':'#6a7078',
 'n':'#7a4a28','N':'#4a2c18','f':'#f08020','F':'#f8e060','u':'#2f6aa8','U':'#6aa8e0','t':'#d8b860',
 'T':'#a88838','p':'#5a4030','P':'#3a2818','v':'#6a3a88','V':'#44205a','c':'#a82820','C':'#701810',
 'e':'#8a6a48','E':'#6a5038','x':'#3a3a38','z':'#b85a30','Z':'#8a3a20',
}
def P(*rows):
    out=[]
    for r in rows: out.append((r+'.'*16)[:16])
    while len(out)<16: out.insert(0,'.'*16)
    return out
S={}
MAN=P("................","......kkkk......",".....khhhhk.....","....khhhhhhk....","....khsssshk....","....kskssksk....","....kssssssk....",".....kSssSk.....","...kkcccccckk...","..kcCccccccCck..","..kcCccccccCck..","..ksCcnnnncCsk..","...kkCccccCkk...",".....kppppk.....",".....kpkkpk.....",".....kNkkNk.....")
WOM=P("................","......kkkk......",".....khhhhk.....","....khhhhhhk....","...khhsssshhk...","...khsksskshk...","...khsssssshk...","...khkSssSkhk...","...khcccccchk...","..kscCccccCcsk..","...kcCccccCck...","...kcwwwwwwck...","..kcCwwwwwwCck..","..kCccccccccCk..","..kCCccccccCCk..","...kkkkkkkkkk...")
KID=P("................","................","................","................","......kkkk......",".....khhhhk.....",".....khsshk.....",".....kksskk.....",".....kssssk.....","....kcccccck....","...kscCccCcsk...","....kcCccCck....","....kcccccck....",".....kpkkpk.....",".....kNkkNk.....","................")
def recolor(g,m): return [''.join(m.get(ch,ch) for ch in r) for r in g]
def overlay(g,pts):
    g=[list(r) for r in g]
    for (x,y,ch) in pts: g[y][x]=ch
    return [''.join(r) for r in g]
S['ch_peasant']=MAN
S['ch_baker']=recolor(WOM,{'h':'H','c':'o','C':'z'})
S['ch_smith']=overlay(recolor(MAN,{'h':'N','c':'W','C':'x'}),[(x,y,'n') for y in (9,10,11,12) for x in range(6,10)])
S['ch_guard']=overlay(recolor(MAN,{'h':'M','c':'b','C':'B'}),[(13,y,'N') for y in range(3,16)]+[(13,1,'m'),(13,2,'m'),(12,2,'k'),(14,2,'k'),(13,0,'k')]+[(x,y,'y') for y in (9,10) for x in (7,8)])
S['ch_priest']=recolor(WOM,{'h':'V','c':'v','C':'V','w':'v'})
S['ch_elder']=recolor(WOM,{'h':'g','c':'G','C':'D','w':'e'})
S['ch_child']=recolor(KID,{'h':'H','c':'r','C':'R'})
S['ch_stranger']=recolor(MAN,{'h':'P','c':'E','C':'p','s':'S'})
S['ob_tree']=P("................",".....kkkkkk.....","...kkDGGGGDkk...","..kDGGllGGGDk...",".kDGGlllGGGGDk..",".kGGllGGGGlGGk..","kDGGGGGGDGllGGk.","kGGlGGGDDGGGGDk.","kGllGGGGGGGlGGk.","kDGGGGlGGGGGGDk.","kDGGGllGGGDGGDk.",".kDGGGGGGDGGDk..",".kDDGGDGGGGDDk..","..kDDDGGDDDDk...","...kkDDDDDkk....",".....kkkkk......")
S['ob_pine']=recolor(S['ob_tree'],{'G':'D','l':'G','D':'K'})
S['ob_rock']=P("................","................","................","................","................","................",".....kkkkk......","....kWWmWWk.....","...kWmmWWWWk....","..kWWWWWWMWWk...","..kMWWWWMMWMk...","..kMMWWMMMMMk...","...kkMMMMMkk....",".....kkkkk......","................","................")
S['ob_bed']=P("................","...kkkkkkkkkk...","...knnnnnnnnk...","...knwwwwwwnk...","...knwwwwwwnk...","...knrrrrrrnk...","...knrRrrRrnk...","...knrrrrrrnk...","...knrRrrRrnk...","...knrrrrrrnk...","...knrRrrRrnk...","...knrrrrrrnk...","...knrrrrrrnk...","...knrrrrrrnk...","...knnnnnnnnk...","...kkkkkkkkkk...")
S['ob_table']=P("................","................","................","................","..kkkkkkkkkkkk..","..knnnnnnnnnnk..","..knNnnnnnnNnk..","..knnnwwnnnnnk..","..knnnwwnnnnnk..","..knnnnnnnnnnk..","..kkkkkkkkkkkk..","..kN........Nk..","..kk........kk..","................","................","................")
S['ob_hearth']=P("................","................","................","...kkkkkkkkkk...","...kMMMMMMMMk...","...kMkkkkkkMk...","...kMkFfFfkMk...","...kMkfFFfkMk...","...kMkfFFfkMk...","...kMknffnkMk...","...kMkkkkkkMk...","...kMMMMMMMMk...","...kkkkkkkkkk...","................","................","................")
S['ob_anvil']=P("................","................","................","................","................","................","................","..kkkkkkkkkkk...",".kMmmmmmmmmMk...","..kkMMMMMMMkk...","....kMMMMMk.....","....kMMMMMk.....","...kMMMMMMMk....","...kkkkkkkkk....","................","................")
S['ob_barrel']=P("................","................","................",".....kkkkkk.....","....knnnnnnk....","...knNnnnnNnk...","...kMMMMMMMMk...","...knnnnnnnnk...","...knNnnnnNnk...","...knnnnnnnnk...","...kMMMMMMMMk...","...knNnnnnNnk...","....knnnnnnk....",".....kkkkkk.....","................","................")
S['ob_chest']=P("................","................","................","................","................","..kkkkkkkkkkkk..","..knnnnnnnnnnk..","..kMnnnnnnnnMk..","..kkkkkkkkkkkk..","..knnnnyynnnnk..","..kMnnnyynnnMk..","..knnnnnnnnnnk..","..kkkkkkkkkkkk..","................","................","................")
S['ob_well']=P("................","................","................",".....kkkkkk.....","...kkWmWWmWkk...","..kWWkkkkkkWWk..","..kWkuUuuuukWk..","..kmkuuuUuukmk..","..kWkuuuuUukWk..","..kWWkkkkkkWWk..","...kkWWmWWWkk...",".....kkkkkk.....","................","................","................","................")
S['ob_sack']=P("................","................","................","................",".......kk.......","......kNNk......",".....kttttk.....","....kttTtttk....","...kttttttTtk...","...kttTttttTk...","...ktttttTttk...","....kTttttTk....",".....kkkkkk.....","................","................","................")
S['ob_bush']=recolor(P("................","................","................","................","................","....kkkkkkk.....","..kkGGlGGDkk....",".kGGllGGGGDGk...",".kGlGGGDGlGDk...",".kDGGGGGGGDDk...","..kkDDGDDDkk....","....kkkkkk......","................","................","................","................"),{})
S['ob_stall']=P("................","kkkkkkkkkkkkkkkk","krwrwrwrwrwrwrwk","krwrwrwrwrwrwrwk","kkkkkkkkkkkkkkkk",".kN..........Nk.",".kN..........Nk.",".kkkkkkkkkkkkkk.",".knnnnnnnnnnnnk.",".knoyonntGnnwnk.",".knyonntGlnwwnk.",".knnnnnnnnnnnnk.",".kkkkkkkkkkkkkk.",".kN..........Nk.",".kk..........kk.","................")
S['it_bread']=P("................","................","................","................","................","................",".....kkkkkk.....","...kkoyyyyokk...","..koyyoyyoyyok..","..kooooooooook..","..kNooooooooNk..","...kkkkkkkkkk...","................","................","................","................")
S['it_sword']=P(".............kk.","............kmMk","...........kmMk.","..........kmMk..",".........kmMk...","........kmMk....",".......kmMk.....","......kmMk......",".....kmMk.......","...k.kmMk.......","..kykkMk........","...kyyk.........","..knkkyk........",".knk..kyk.......","kyk.............",".k..............")
S['it_ore']=recolor(overlay(S['ob_rock'],[(5,8,'o'),(6,8,'z'),(9,9,'o'),(10,10,'z'),(4,10,'o'),(7,11,'o')]),{})
S['it_ingot']=P("................","................","................","................","................","................","................","................","....kkkkkkkk....","...kmmmmmmmMk...","..kmmmmmmmmMMk..","..kMMMMMMMMMMk..","...kkkkkkkkkk...","................","................","................")
S['it_coin']=P("................","................","................","................",".....kkkkkk.....","....kyyyyyyk....","...kyyoooyyyk...","...kyoyyyyoyk...","...kyoyyyyoyk...","...kyyoooyyyk...","....kyyyyyyk....",".....kkkkkk.....","................","................","................","................")
S['it_herb']=P("................","................","................","................",".......kk.......","......klGk......","..kk..klGk..kk..",".klGk.kGk..kGlk.","..kGGkkGk.kGGk..","...kGGkGkkGGk...","....kkGGGGkk....","......kGGk......","......kDDk......","......kkkk......","................","................")
S['it_ale']=P("................","................","................","................","....kkkkkk......","...kwwwwwwk.....","...knyyyynkkk...","...knyyyynk.k...","...knyyyynk.k...","...knyyyynkkk...","...knnnnnnk.....","...kkkkkkkk.....","................","................","................","................")
S['it_hammer']=P("................","................","................","....kkkkkkk.....","...kMmmmmMk.....","...kMMMMMMk.....","....kkknkk......","......knk.......","......knk.......","......knk.......","......knk.......","......knk.......","......knk.......","......knk.......","......kNk.......","......kkk.......")
S['it_wheat']=P("................","....k..k..k.....","...kyk.kyk.kyk..","...kyk.kyk.kyk..","...kTk.kyk.kTk..","....kk.kTk.kk...","....kTkkTkkTk...",".....kTTTTTk....","......kTTTk.....","......knnnk.....","......kTTTk.....",".....kTkTkTk....",".....kTkTkTk....","......k.k.k.....","................","................")
S['it_apple']=P("................","................","................","................",".......kk.......","......kNkkk.....",".....kkNklk.....","....krrrrrrk....","...krwrrrrrRk...","...krwrrrrrRk...","...krrrrrrRRk...","....krrrrRRk....",".....kkkkkk.....","................","................","................")
# тайлы процедурно
def tile(base, specks, seed, extra=None):
    rnd=random.Random(seed); g=[[base]*16 for _ in range(16)]
    for ch,n in specks:
        for _ in range(n): g[rnd.randrange(16)][rnd.randrange(16)]=ch
    if extra: extra(g,rnd)
    return [''.join(r) for r in g]
def planks(g,r):
    for y in range(16):
        for x in range(16):
            if x%4==3: g[y][x]='N'
        if y in (3,11): 
            for x in range(0,16,4): g[y][x+ (0 if y==3 else 2)]='N'
def bricks(g,r):
    for y in range(16):
        for x in range(16):
            if y%4==3 or (x+(4 if (y//4)%2 else 0))%8==7: g[y][x]='x'
def cobble(g,r):
    for y in range(16):
        for x in range(16):
            if y%5==4 or (x+(3 if (y//5)%2 else 0))%6==5: g[y][x]='E'
def water(g,r):
    for (y,x0) in ((3,2),(8,9),(12,4)):
        for x in range(x0,x0+4): g[y][x%16]='U'
def crops(g,r):
    for y in range(16):
        for x in range(16):
            if x%4 in (1,2) and y%3!=2: g[y][x]='G' if (x+y)%5 else 'l'
def roof(g,r):
    for y in range(16):
        for x in range(16):
            if (x+y)%4==0: g[y][x]='Z'
            if y%8==7: g[y][x]='C'
T={}
T['tl_grass']=tile('G',[('l',22),('D',18)],1)
T['tl_grass2']=tile('G',[('l',10),('D',26),('y',2)],2)
T['tl_dirt']=tile('e',[('E',24),('t',6)],3)
T['tl_road']=tile('W',[('m',10)],4,cobble)
T['tl_water']=tile('u',[('B',10)],5,water)
T['tl_floor']=tile('n',[('e',8)],6,planks)
T['tl_wall']=tile('W',[('m',8),('M',6)],7,bricks)
T['tl_field']=tile('e',[('E',10)],8,crops)
T['tl_roof']=tile('z',[('o',8)],9,roof)
T['tl_sand']=tile('t',[('T',16),('y',4)],10)
def door(g,r):
    for y in range(1,15):
        for x in range(4,12): g[y][x]='n' if x%3 else 'N'
    for x in range(3,13): g[0][x]='k'; g[15][x]='k'
    for y in range(16): g[y][3]='k'; g[y][12]='k'
T['tl_door']=tile('W',[],11,lambda g,r:(bricks(g,r),door(g,r)))
S.update(T)
def svg_symbol(name,grid,px=1):
    rects=[]
    for y,row in enumerate(grid):
        x=0
        while x<16:
            ch=row[x]
            if ch=='.' : x+=1; continue
            x2=x
            while x2<16 and row[x2]==ch: x2+=1
            rects.append(f'<rect x="{x}" y="{y}" width="{x2-x}" height="1" fill="{PAL[ch]}"/>')
            x=x2
    return f'<symbol id="{name}" viewBox="0 0 16 16">'+''.join(rects)+'</symbol>'
def defs():
    return '<svg width="0" height="0" style="position:absolute"><defs>'+''.join(svg_symbol(k,v) for k,v in S.items())+'</defs></svg>'
def standalone(name):
    inner=svg_symbol(name,S[name]).replace(f'<symbol id="{name}" viewBox="0 0 16 16">','').replace('</symbol>','')
    return f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16" width="64" height="64" shape-rendering="crispEdges">{inner}</svg>'
