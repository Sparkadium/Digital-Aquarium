// Procedural Abyss for the non-touch 172x320 / 320x172 display.
// Port of the approved browser preview: 6 lanternfish, 3 hatchetfish,
// 1 stoplight loosejaw and 1 small angler. No bitmap assets.
// Included by tank.h inside namespace gt. No frame-time allocations.
#pragma once

struct AbyssSpec {
  uint8_t segments;
  float spacing, radius[6], speed, school, turn, lo, hi;
  RGB8 body;
};
static const AbyssSpec AS[4] = {
  {4, 1.6f, {1.5f,1.8f,1.5f,.9f,0,0}, 13.f,.8f,2.6f,30,262, {34,42,56}},
  {3, 1.7f, {1.8f,3.5f,1,0,0,0}, 6.f,.3f,1.8f,40,220, {35,50,61}},
  {6, 1.8f, {1.5f,1.7f,1.5f,1.1f,.7f,.45f}, 5.f,0,1.35f,95,245, {29,28,39}},
  {6, 2.4f, {5,6,5.5f,4,2.4f,1.4f}, 5.f,0,1.f,120,255, {34,30,44}}
};
static uint32_t abyssLastFeed = 0;
static uint16_t planktonDriftRemainder = 0;

// Sparse, coloured radial kernels. Weights generated from the same equations
// as the browser: max(0, 1 - distance/(radius+.5))^power. Flash, not RAM.
#include "abyss_kernels.h"
static void abyssGlowQ(int xq, int yq, int which, RGB8 col, int amp) {
  if (amp <= 0) return;
  const int x = (xq+8)>>4, y = (yq+8)>>4;
  for (int i=AG_OFF[which]; i<AG_OFF[which+1]; ++i) {
    const AbyssGlowPoint &p=AG_POINTS[i];
    const int q=(p.weight*amp+128)>>8;
    if (q) addPx(x+p.x,y+p.y,col.r,col.g,col.b,q);
  }
}
static inline void abyssGlow(float x,float y,int which,RGB8 c,int amp) {
  abyssGlowQ((int)(x*16.f),(int)(y*16.f),which,c,amp);
}
static void disc(float,float,float,RGB8,int); // shared integer coverage rasterizer
static inline void abyssPixel(float x,float y,RGB8 c,int alpha) {
  blendPx(ifloor(x+.5f),ifloor(y+.5f),c,alpha);
}
static void abyssLine(float x0,float y0,float x1,float y1,RGB8 c,int alpha) {
  int x=(int)(x0*256.f), y=(int)(y0*256.f);
  const int dx=(int)(x1*256.f)-x, dy=(int)(y1*256.f)-y;
  const int n=((abs(dx)>abs(dy)?abs(dx):abs(dy))+255)/256+1;
  const int sx=dx/n,sy=dy/n;
  for (int i=0;i<=n;i++,x+=sx,y+=sy) blendPx((x+128)>>8,(y+128)>>8,c,alpha);
}
struct AbyssPoint { int16_t x,y; }; // Q4
static void abyssPolygon(const AbyssPoint *p,int n,RGB8 c,int alpha) {
  int ymin=H*16,ymax=0;
  for(int i=0;i<n;i++) { if(p[i].y<ymin)ymin=p[i].y; if(p[i].y>ymax)ymax=p[i].y; }
  for(int y=clampi((ymin+15)>>4,0,H-1); y<=clampi(ymax>>4,0,H-1); y++) {
    int xs[8],count=0,yq=y*16;
    for(int i=0,j=n-1;i<n;j=i++) {
      const AbyssPoint &a=p[j],&b=p[i];
      if((a.y<=yq && b.y>yq)||(b.y<=yq && a.y>yq))
        xs[count++]=a.x+(yq-a.y)*(b.x-a.x)/(b.y-a.y);
    }
    for(int i=1;i<count;i++) { int v=xs[i],j=i;while(j>0&&xs[j-1]>v){xs[j]=xs[j-1];j--;}xs[j]=v; }
    for(int i=0;i+1<count;i+=2) {
      int lo=(xs[i]+15)>>4,hi=xs[i+1]>>4;
      if(lo<0)lo=0;if(hi>=W)hi=W-1;
      for(int x=lo;x<=hi;x++)blendPx(x,y,c,alpha);
    }
  }
}
static inline void syncAbyssFish(AbyssFish &f) {
  fsincos(f.h,f.hs,f.hc); f.qx=(uint16_t)(f.x*128.f); f.qy=(uint16_t)(f.y*128.f);
}
static void initAbyss() {
  new (&creatures.abyss) AbyssState(); // activate the shared storage; no allocation
  for(int i=0;i<ABYSS_FISH_COUNT;i++) {
    AbyssFish &f=abyssFish[i]; f=AbyssFish();
    f.sp=i<6?A_LANTERN:i<9?A_HATCHET:i==9?A_LOOSEJAW:A_ANGLER;
    const AbyssSpec &s=AS[f.sp];
    f.x=rnd(15.f,W-15.f); f.y=rnd(layoutY(s.lo)+5.f,layoutY(s.hi)-5.f);
    f.h=frand()<.5f?0.f:PIF; f.v=s.speed*.5f;
    f.phase=rnd(0,TAUF); f.speedScale=rnd(.8f,1.2f);
    syncAbyssFish(f);
  }
  static const float starts[4][3]={{34,80,0},{101,112,PIF},{126,55,0},{126,170,PIF}};
  for(int i=0;i<4;i++) { AbyssFish &f=abyssFish[i+6];f.x=layoutX(starts[i][0]);f.y=layoutY(starts[i][1]);f.h=starts[i][2];syncAbyssFish(f); }
  for(int i=0;i<ABYSS_PLANKTON_COUNT;i++) plankton[i]={(uint16_t)(xr()%(W*128)),(uint16_t)(13*128+xr()%((int)(WATER_BOT-15.f)*128)),(uint16_t)xr(),0};
  for(int i=0;i<70;i++) motes[i]={rnd(0,W),rnd(SURFACE,WATER_BOT),rnd(.2f,1.f),rnd(0,TAUF)};
  abyssLastFeed=tms-1200u; planktonDriftRemainder=0;
}
static void abyssFeed(float x) {
  if((uint32_t)(tms-abyssLastFeed)<1200u) return;
  int active=0; for(int i=0;i<MAXFOOD;i++) if(food[i].on) active++;
  if(active>=48) return;
  if(x<0) x=rnd(20.f,W-20.f);
  x=clampf(x,12.f,W-12.f); abyssLastFeed=tms;
  const int count=GT_FLAKES_PER_PINCH<48-active?GT_FLAKES_PER_PINCH:48-active;
  for(int i=0,added=0;i<MAXFOOD&&added<count;i++) if(!food[i].on) {
    Food &q=food[i];q=Food();q.on=true;q.serial=foodSerial++;
    q.x=x+rnd(-8.f,8.f);q.y=SURFACE+rnd(0,3.f);q.vy=rnd(4.f,8.f);q.phase=rnd(0,TAUF);q.life=1.f;
    syncFood(q);added++;
  }
  const int xq=(int)(x*128.f),yq=(int)(layoutY(40.f)*128.f);
  for(int i=0;i<ABYSS_PLANKTON_COUNT;i++) {
    Plankton &p=plankton[i];const int dx=(int)p.x-xq,dy=(int)p.y-yq;
    if(abs(dx)>=34*128||abs(dy)>=34*128)continue;
    const int d2=dx*dx+dy*dy;
    if(d2<34*34*128*128) {
      const int e=(int)(65535.f-sqrtf((float)d2)*(65535.f/(40.f*128.f)));
      if(e>p.energy)p.energy=(uint16_t)e;
    }
  }
}
static void updateAbyssFish(int index,float dt) {
  AbyssFish &f=abyssFish[index];const AbyssSpec &s=AS[f.sp];
  f.wander=clampf(f.wander*(1.f-.45f*dt)+rnd(-1.5f,1.5f)*dt,-.9f,.9f);
  float sx,sy;fsincos(f.h+f.wander,sy,sx);
  if(f.y<layoutY(s.lo)+8)sy+=(layoutY(s.lo)+8-f.y)*.06f;
  if(f.y>layoutY(s.hi)-8)sy-=(f.y-(layoutY(s.hi)-8))*.06f;
  if(f.x<14)sx+=(14-f.x)*.08f;
  if(f.x>W-14)sx-=(f.x-(W-14))*.08f;
  float cx=0,cy=0,ax=0,ay=0;int neighbours=0;
  for(int i=0;i<ABYSS_FISH_COUNT;i++) {
    if(i==index)continue;
    const AbyssFish &o=abyssFish[i];
    if(abs(f.qx-o.qx)>30*128||abs(f.qy-o.qy)>30*128)continue;
    const float dx=o.x-f.x,dy=o.y-f.y,d2=dx*dx+dy*dy;
    if(d2>900.f)continue;
    const float md=3.f+s.radius[1]+AS[o.sp].radius[1];
    if(d2<md*md&&d2>.0001f) { const float d=sqrtf(d2),k=(md-d)*.12f/d;sx-=dx*k;sy-=dy*k; }
    if(o.sp==f.sp&&s.school>0) {cx+=o.x;cy+=o.y;ax+=o.hc;ay+=o.hs;neighbours++;}
  }
  if(neighbours) { const float inv=1.f/neighbours;sx+=((cx*inv-f.x)*.02f+ax*inv*.6f)*s.school;sy+=((cy*inv-f.y)*.02f+ay*inv*.6f)*s.school; }
  const float range=f.sp==A_LOOSEJAW?90.f:f.sp==A_HATCHET?80.f:110.f;
  float best=range*range;int target=-1;
  for(int i=0;i<MAXFOOD;i++) if(food[i].on) {
    const float dx=food[i].x-f.x,dy=food[i].y-f.y,d2=dx*dx+dy*dy;
    if(d2<best){best=d2;target=i;}
  }
  f.focus+=((target>=0?1.f:0.f)-f.focus)*dt*2.f;
  float speed=s.speed;
  if(target>=0) {
    Food &q=food[target];const float d=sqrtf(best)+.001f;
    sx+=(q.x-f.x)/d*3.f;sy+=(q.y-f.y)/d*3.f;
    speed*=f.sp==A_LOOSEJAW?2.15f:1.8f;
    if(d<2.5f+s.radius[1]*.5f){q.on=false;eaten++;}
  }
  float a=fatan2(sy,sx),lim=target>=0?1.25f:.85f;
  if(a>lim&&a<PIF-lim)a=a<PIF*.5f?lim:PIF-lim;
  if(a<-lim&&a>-PIF+lim)a=a>-PIF*.5f?-lim:-PIF+lim;
  f.h=wrapA(f.h+clampf(wrapA(a-f.h),-s.turn*dt,s.turn*dt));
  f.v+=(speed*f.speedScale-f.v)*dt*2.f;fsincos(f.h,f.hs,f.hc);
  f.x=clampf(f.x+f.hc*f.v*dt,3.f,W-3.f);f.y=clampf(f.y+f.hs*f.v*dt,SURFACE+3.f,WATER_BOT-2.f);
  f.phase+=dt*(5.f+f.v*.35f);if(f.phase>=TAUF)f.phase-=TAUF;
  f.qx=(uint16_t)(f.x*128.f);f.qy=(uint16_t)(f.y*128.f);
}
static void stepAbyss(uint32_t dtMs) {
  if(!dtMs)return;
  const float dt=dtMs*.001f;
  uint16_t foodX[MAXFOOD],foodY[MAXFOOD];int nFood=0;
  for(int i=0;i<MAXFOOD;i++) if(food[i].on) {
    Food &q=food[i];q.y+=q.vy*dt*depthScale;
    q.x=clampf(q.x+isin((uint16_t)(tph(K16(.0015))+bam(q.phase)))*(3.f/16384.f)*dt,3.f,W-3.f);
    const float floor=floorAt(q.x)-2.f;
    if(q.y>=floor){q.y=floor;q.floatT+=dt;if(q.floatT>12.f){q.on=false;continue;}}
    syncFood(q);
  }
  for(int i=0;i<ABYSS_FISH_COUNT;i++)updateAbyssFish(i,dt);
  for(int i=0;i<MAXFOOD;i++)if(food[i].on){foodX[nFood]=(uint16_t)(food[i].x*128.f);foodY[nFood]=(uint16_t)(food[i].y*128.f);nFood++;}
  // Shared drift remainder keeps vertical motion independent of frame rate.
  const int advance=dtMs*102+planktonDriftRemainder;
  const int yStep=advance/1000;planktonDriftRemainder=advance%1000;
  const uint16_t wave=tph(K16(.0004));
  const uint32_t decay=AG_DECAY[dtMs];
  const uint32_t chance=dtMs*118u/10u; // .18 flashes per point per second, Q16
  for(int i=0;i<ABYSS_PLANKTON_COUNT;i++) {
    Plankton &p=plankton[i];p.energy=(uint16_t)((p.energy*decay)>>16);
    int px=(int)p.x+(isin((uint16_t)(wave+p.phase))*(int)dtMs*77)/(16384*1000);
    if(px<0)px+=W*128;if(px>=W*128)px-=W*128;p.x=(uint16_t)px;
    p.y+=yStep;if(p.y>(int)(WATER_BOT-2.f)*128){p.y=13*128;p.x=(uint16_t)(xr()%(W*128));}
    const uint32_t noise=xr();
    if((noise&65535u)<chance){const int e=16384+(noise>>18);if(e>p.energy)p.energy=(uint16_t)e;}
    for(int j=0;j<ABYSS_FISH_COUNT;j++) {
      const AbyssFish &f=abyssFish[j];const int r=f.sp==A_ANGLER?14*128:f.sp==A_HATCHET?1472:1255;
      const int dx=f.qx-(int)p.x,dy=f.qy-(int)p.y;
      if(abs(dx)<r&&abs(dy)<r&&dx*dx+dy*dy<r*r){if(p.energy<58982)p.energy=58982;break;}
    }
    if(p.energy<45875)for(int j=0;j<nFood;j++) {
      const int dx=foodX[j]-(int)p.x,dy=foodY[j]-(int)p.y;
      if(abs(dx)<512&&abs(dy)<512&&dx*dx+dy*dy<512*512){p.energy=45875;break;}
    }
  }
#if GT_SNOW
  for(int i=0;i<70;i++) {
    Mote &m=motes[i];m.y+=3.f*m.z*dt;m.x+=isin((uint16_t)(tph(K16(.0005))+bam(m.ph)))*(2.f/16384.f)*dt;
    if(m.x<0)m.x+=W;if(m.x>=W)m.x-=W;if(m.y>WATER_BOT){m.y=SURFACE+1.f;m.x=rnd(0,W);}
  }
#endif
}

static void abyssBakeRow(int y,uint16_t *dst) {
  uint16_t *row=dst+y*W;
  const int f=clampi((y-9)*256/(int)(WATER_BOT-SURFACE),0,256);
  const int r0=((3*(256-f))*109)>>16,g0=((10*256-9*f)*109)>>16,b0=((22*256-18*f)*109)>>16;
  for(int x=0;x<W;x++) {
    int r=r0,g=g0,b=b0;
    if(y<9){r=2;g=5;b=9;}
    if(y*16>=floorQ4[x]) {const int v=6+(int)(hh(x/3,y/3)%6);r=v;g=v;b=v+2;}
    // Keep the approved preview's two small, barely-lit seabed rocks.
    const int centers[2]={layoutXi(34),layoutXi(132)},rx[2]={17,14},ry[2]={11,9};
    for(int i=0;i<2;i++) {const int dx=x-centers[i],dy=y-layoutYi(280);
      if(dx*dx*ry[i]*ry[i]+dy*dy*rx[i]*rx[i]<rx[i]*rx[i]*ry[i]*ry[i]){r=9-dy/4;g=r;b=r+3;}}
    const int v=256-((x-VCX)*(x-VCX)*90)/(VCX*VCX);
    row[x]=pack565d((r*v)>>8,(g*v)>>8,(b*v)>>8,x,y);
  }
}
static void drawAbyssHatchet(const AbyssFish &f) {
  const float ux=f.hc,uy=f.hs,dx=uy*(ux<0?1.f:-1.f),dy=fabsf(ux);
  static const int8_t shape[8][2]={{22,-13},{-11,-46},{-74,-32},{-102,-8},{-90,32},{-58,66},{-10,67},{22,21}};
  AbyssPoint p[8];
  for(int i=0;i<8;i++)p[i]={(int16_t)(f.x*16.f+ux*shape[i][0]+dx*shape[i][1]),(int16_t)(f.y*16.f+uy*shape[i][0]+dy*shape[i][1])};
  abyssPolygon(p,8,{35,50,61},243);
  const float wag=fsin(f.phase)*.45f;
  for(int side=-1;side<=1;side+=2)abyssLine(f.x-ux*5.8f,f.y-uy*5.8f,f.x-ux*8.8f+dx*(side*1.65f+wag),f.y-uy*8.8f+dy*(side*1.65f+wag),{35,61,75},154);
  abyssLine(f.x-ux*4.6f-dx*2.f,f.y-uy*4.6f-dy*2.f,f.x-ux*.7f-dx*2.9f,f.y-uy*.7f-dy*2.9f,{65,87,103},115);
  disc(f.x+ux*.1f-dx,f.y+uy*.1f-dy,.65f,{7,15,23},256);
  static const float photo[4][2]={{-4.5f,2.1f},{-3.1f,3.3f},{-1.5f,3.65f},{.1f,2.6f}};
  const int pulse=238+(isin((uint16_t)(tph(K16(.0008))+bam(f.phase*.1f)))*18>>14);
  for(int i=0;i<4;i++) {
    const float x=f.x+ux*photo[i][0]+dx*photo[i][1],y=f.y+uy*photo[i][0]+dy*photo[i][1];
    abyssGlow(x,y,0,{160,218,255},(110*pulse)>>8);abyssGlow(x,y,1,{62,132,190},17);
    addPx(ifloor(x+.5f),ifloor(y+.5f),60,76,86,256);
  }
}
static void drawAbyssFish(const AbyssFish &f) {
  if(f.sp==A_HATCHET){drawAbyssHatchet(f);return;}
  const AbyssSpec &s=AS[f.sp];const int n=s.segments;
  const float ux=f.hc,uy=f.hs,dx=uy*(ux<0?1.f:-1.f),dy=fabsf(ux);
  float px[6],py[6],x=f.x,y=f.y,ang=f.h;
  for(int i=0;i<n;i++) {px[i]=x;py[i]=y;ang=f.h+fsin(f.phase-i*.9f)*(.22f*i/(n-1));float sn,cs;fsincos(ang,sn,cs);x-=cs*s.spacing;y-=sn*s.spacing;}
  for(int side=-1;side<=1;side+=2) {
    const float a=ang+side*.45f,len=s.radius[1]*1.3f+1.f;
    abyssLine(px[n-1],py[n-1],px[n-1]-fcos(a)*len,py[n-1]-fsin(a)*len,s.body,179);
  }
  for(int i=n-1;i>=0;i--)disc(px[i],py[i],s.radius[i],s.body,243);
  if(f.sp==A_LANTERN) {
    for(int i=0;i<n-1;i++){const float gx=px[i]+dx*s.radius[i]*.8f,gy=py[i]+dy*s.radius[i]*.8f;
      abyssGlow(gx,gy,0,{60,200,255},82);abyssGlow(gx,gy,1,{40,160,255},26);}
  } else if(f.sp==A_LOOSEJAW) {
    const int points[3]={2,4,5};
    for(int j=0;j<3;j++){int i=points[j];abyssGlow(px[i]+dx*s.radius[i]*.75f,py[i]+dy*s.radius[i]*.75f,0,{45,135,215},51);}
    const float hx=f.x+ux*.4f+dx*.65f,hy=f.y+uy*.4f+dy*.65f;
    const int pulse=(int)(200.f+56.f*f.focus);
    abyssGlow(hx,hy,0,{255,36,29},pulse);abyssGlow(hx,hy,1,{205,20,18},(44*pulse)>>8);
    addPx(ifloor(hx+.5f),ifloor(hy+.5f),44,6,4,256);
    abyssGlow(f.x-ux*1.6f-dx*.65f,f.y-uy*1.6f-dy*.65f,0,{45,162,212},49);
    const float jaw=.9f+f.focus*.7f;
    abyssLine(f.x+ux*1.5f+dx*jaw,f.y+uy*1.5f+dy*jaw,f.x-ux*2.f+dx*2.f,f.y-uy*2.f+dy*2.f,{50,33,40},128);
  } else {
    abyssPixel(f.x+ux*.7f-dx*.7f,f.y+uy*.7f-dy*.7f,{8,8,12},230);
    const int pulse=179+(isin(tph(K16(.0023)))*77>>14);
    const float bob=isin(tph(K16(.0017)))*(1.2f/16384.f),tx=f.x+ux*8.f-dx*8.f,ty=f.y+uy*8.f-dy*8.f+bob;
    abyssLine(f.x-dx*4.f,f.y-dy*4.f,tx,ty,{70,66,84},154);
    abyssPixel(f.x+ux*4.4f+dx*1.5f,f.y+uy*4.4f+dy*1.5f,{210,210,200},205);
    abyssPixel(f.x+ux*4.2f+dx*2.8f,f.y+uy*4.2f+dy*2.8f,{210,210,200},179);
    abyssGlow(tx,ty,0,{150,255,230},(230*pulse)>>8);abyssGlow(tx,ty,2,{70,210,190},(82*pulse)>>8);
    abyssGlow(f.x+ux*2.f,f.y+uy*2.f,1,{30,70,64},(90*pulse)>>8);
  }
}
static void renderAbyss() {
  for(int i=0;i<MAXFOOD;i++)if(food[i].on) {
    abyssGlow(food[i].x,food[i].y,0,{255,200,110},128);abyssGlow(food[i].x,food[i].y,1,{255,160,70},26);
  }
  for(int i=0;i<ABYSS_FISH_COUNT;i++)drawAbyssFish(abyssFish[i]);
  for(int i=0;i<ABYSS_PLANKTON_COUNT;i++) {
    const Plankton &p=plankton[i];const int e=p.energy>>8;
    if(e<5)continue;
    addPx((p.x+64)>>7,(p.y+64)>>7,60,240,220,e);
    if(e>89)abyssGlowQ(p.x>>3,p.y>>3,0,{40,220,200},(e*102)>>8);
    if(e>153)abyssGlowQ(p.x>>3,p.y>>3,1,{20,160,170},(e*31)>>8);
  }
#if GT_SNOW
  for(int i=0;i<70;i++){const Mote &m=motes[i];addPx(ifloor(m.x+.5f),ifloor(m.y+.5f),34,44,56,(int)(m.z*358.f));}
#endif
}
