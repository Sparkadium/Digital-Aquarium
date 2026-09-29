#pragma once
// Runtime geometry, no bitmaps, downloaded artwork, or image assets.
// Projection, rasterization and per-pixel shading use fixed point. The small
// floating-point simulation runs concurrently with the existing SPI DMA push.
static inline RGB8 pkColor(uint32_t c){return {(uint8_t)(c>>16),(uint8_t)(c>>8),(uint8_t)c};}
static inline int pkQ(float x){return (int)(x*16.f);}
static inline float pkGround(float x){x=referenceX(x);return (H-27.f)+4.f*fsin(x*.041f)+2.f*fsin(x*.12f);}
struct PKSpec { float home,range,speed,sense,scale; };
static const PKSpec PK_SPEC[PK_KINDS]={
  {51,25,3.8f,56,.82f},{99,58,7.5f,103,.83f},{137,70,10.7f,116,.9f},
  {190,54,5.5f,92,.86f},{173,70,10.1f,105,.82f},{282,7,4.7f,50,.91f},{280,12,3.2f,51,.95f}
};
static inline PokemonState &pk(){return creatures.pokemon;}
static inline void pkSync(PokemonFish &f){f.qx=(int16_t)pkQ(f.x);f.qy=(int16_t)pkQ(f.y);}
static void initPokemon(){
  new (&creatures.pokemon) PokemonState();
  pk().pixels=fb;
  static const float X[7]={69,122,53,39,114,62,131},Y[7]={43,88,113,198,191,281,280};
  for(int c=0;c<GT_POKEMON_COPIES;c++)for(int s=0;s<PK_KINDS;s++){
    int i=c*7+s;auto &f=pk().fish[i]; const auto &sp=PK_SPEC[s];f.kind=s;
    f.x=clampf(c==0?layoutX(X[s]): (c==1?W-layoutX(X[s]):layoutX(30.f+s*18.f)),22,W-22);
    f.y=clampf(layoutY(Y[s])+(s<5?c*18.f*depthScale:0.f),landscape?22.f:28.f,s<5?layoutY(259.f):pkGround(f.x)-10.f);
    f.yaw=(i&1)?PIF-.12f:.12f;f.yawTarget=f.tailYaw=f.yaw;
    f.vx=(i&1)?-sp.speed:sp.speed;f.phase=rnd(0,TAUF);f.clock=rnd(2,5);
    f.tx=clampf(f.x+((i&1)?-17:20),25,W-25);f.ty=f.y;f.spin=rnd(-.3f,.3f);
    f.speedScale=rnd(.91f,1.09f);f.target=-1;pkSync(f);
  }
}
static void pokemonFeed(float x){
  if(pk().feedCooldown>0)return;
  int active=0;for(auto&q:food)active+=q.on;
  if(active>30)return; // max 41 pellets, even under repeated button presses
  pk().feedCooldown=.65f;if(x<0)x=rnd(layoutX(63),layoutX(111));
  for(int i=0;i<11;i++)for(auto&q:food)if(!q.on){
    q=Food();q.on=true;q.serial=foodSerial++;q.x=clampf(x+rnd(-layoutX(36),layoutX(36)),20,W-20);
    q.y=rnd(14,22);q.term=(i%3==0)?rnd(23,30):rnd(7,13);q.phase=rnd(0,TAUF);q.life=55;syncFood(q);break;
  }
  addRing(x,1,1.2f);
}
static void stepPokemon(uint32_t dtMs){
  if(!dtMs)return;
  float dt=dtMs*.001f,t=tms*.001f;
  pk().feedCooldown=clampf(pk().feedCooldown-dt,0,1);
  for(auto&q:food)if(q.on){
    q.life-=dt;if(q.life<=0){q.on=false;continue;}
    float bottom=pkGround(q.x)-2;
    if(q.y<bottom){q.y+=q.term*dt*depthScale;q.x+=fsin(t*1.8f+q.phase)*1.6f*dt;}
    if(q.y>=bottom){q.y=bottom;q.state=2;}syncFood(q);
  }
  bool taken[MAXFOOD]={};
  // Rotate priority, so duplicates get a fair chance at the nearest pellet.
  int start=(tms/700)%PK_COUNT;
  for(int n=0;n<PK_COUNT;n++){
    int i=(start+n)%PK_COUNT;auto &f=pk().fish[i];const auto &sp=PK_SPEC[f.kind];bool floor=f.kind>=PK_WOOPER;
    f.clock-=dt;f.cooldown=clampf(f.cooldown-dt,0,7);f.eat=clampf(f.eat-dt*2.3f,0,1);
    f.phase+=dt*(2.5f+(fabsf(f.vx)+fabsf(f.vy))*.19f);if(f.phase>TAUF)f.phase-=TAUF;
    if(f.clock<=0){f.tx=rnd(layoutX(floor?32.f:27.f),layoutX(floor?143.f:145.f));f.ty=clampf(layoutY(sp.home)+rnd(-sp.range*.7f,sp.range*.7f)*depthScale,landscape?22.f:29.f,layoutY(285.f));f.clock=rnd(3,7);}
    f.target=-1;float nearest=sp.sense*sp.sense;int qr=(int)(sp.sense*16);
    if(f.cooldown==0)for(int j=0;j<MAXFOOD;j++){
      auto &q=food[j];if(!q.on||taken[j]||(floor&&q.y<layoutY(249))||(!floor&&q.y>layoutY(274))||(f.kind==PK_TENTACOOL&&q.y>layoutY(104)))continue;
      if(abs(q.qx-f.qx)>qr||abs(q.qy-f.qy)>qr)continue;
      float dx=q.x-f.x,dy=q.y-f.y,d2=dx*dx+dy*dy;
      if(d2<nearest){nearest=d2;f.target=j;}
    }
    bool hunting=f.target>=0;if(hunting)taken[f.target]=true;
    float dx=(hunting?food[f.target].x:f.tx)-f.x,dy=(hunting?food[f.target].y:f.ty)-f.y;
    float distance=sqrtf(dx*dx+dy*dy),speed=sp.speed*f.speedScale*(hunting?1.9f:1.f)*(.85f+.15f*fsin(f.phase*.2f));
    float inv=distance>.01f?speed/distance:0,vx=dx*inv,vy=dy*inv*.8f;
    if(!hunting){
      vy+=fsin(t*.77f+i*1.7f)*.8f;
      if(f.kind==PK_TENTACOOL){float pulse=clampf(fsin(f.phase),0,1);vy+=3.1f-pulse*pulse*10;}
      if(f.kind==PK_HORSEA){vx*=.65f;vy+=fsin(f.phase)*1.1f;}
      if(distance<5){vx*=distance*.2f;vy*=distance*.2f;}
    }
    for(int j=0;j<PK_COUNT;j++){
      if(i==j)continue;const auto&o=pk().fish[j];int axq=f.qx-o.qx,ayq=f.qy-o.qy;
      if(abs(axq)>22*16||abs(ayq)>22*16)continue;
      float ax=f.x-o.x,ay=f.y-o.y,d2=ax*ax+ay*ay;float sep=(f.kind==PK_TENTACOOL||o.kind==PK_TENTACOOL)?22.f:17.f;
      if(d2<sep*sep&&d2>.01f){float d=sqrtf(d2),force=(sep-d)*.85f/d;vx+=ax*force;vy+=ay*force;}
    }
    vx+=clampf(20-f.x,0,30)*2-clampf(f.x-(W-20),0,30)*2;
    float minY=floor?layoutY(262.f):f.kind==PK_TENTACOOL?(landscape?22.f:28.f):29.f,maxY=floor?pkGround(f.x)-10.f:f.kind==PK_TENTACOOL?layoutY(103.f):layoutY(269.f);
    vy+=clampf(minY-f.y,0,80)*2-clampf(f.y-maxY,0,80)*2;
    if(floor)vy+=((hunting?clampf(food[f.target].y,layoutY(266),pkGround(f.x)-10):pkGround(f.x)-10)-f.y)*2;
    if(fabsf(vx)>.8f)f.yawTarget=vx<0?PIF-.12f:.12f;
    float wanted=f.yawTarget+fsin(t*.47f+i*1.7f)*.045f;
    f.turnRate=clampf(f.turnRate+((wanted-f.yaw)*(hunting?14.f:10.f)-f.turnRate*6.4f)*dt,-2.7f,2.7f);
    f.yaw=clampf(f.yaw+f.turnRate*dt,.035f,PIF-.035f);
    // Stable rational damping, avoiding exp() in the software-float hot loop.
    f.tailYaw+=(f.yaw-f.tailYaw)*(dt*3.1f/(1+dt*3.1f));
    f.bank+=(-f.turnRate*.065f-f.bank)*(dt*4/(1+dt*4));
    if(f.kind!=PK_STARYU&&f.kind!=PK_TENTACOOL)vx=fabsf(vx)*fcos(f.yaw);
    if(!floor)vy+=f.turnRate*.9f;
    float k=dt*(hunting?2.4f:1.35f);k=k/(1+k);
    f.vx+=(vx-f.vx)*k;f.vy+=(vy-f.vy)*k;
    f.x=clampf(f.x+f.vx*dt,16,W-16);f.y=clampf(f.y+f.vy*dt,minY-1,maxY+2);pkSync(f);
    if(f.kind==PK_STARYU){f.spin+=dt*(hunting?1.1f:(fabsf(f.vx)+fabsf(f.vy))*.04f);if(f.spin>TAUF)f.spin-=TAUF;}
    if(hunting){auto&q=food[f.target];dx=q.x-f.x;dy=q.y-f.y;
      if(q.on&&dx*dx+dy*dy<(floor?144.f:42.25f)){
        q.on=false;f.eat=1;f.cooldown=rnd(3.8f,6.5f);f.meals++;eaten++;
        for(int k=0;k<5;k++)addSpark(q.x,q.y,rnd(-5,5),rnd(-5,5),1.1f,pkColor(0xffd269));
      }
    }
  }
  for(int i=0;i<80;i++){auto&m=motes[i];m.y+=(.35f+m.z*1.55f)*dt;m.x+=fsin(t*.4f+m.ph)*dt*.4f;if(m.y>H-27){m.y=16;m.x=rnd(0,W);}}
  for(auto&s:sparks)if(s.on){s.x+=s.vx*dt;s.y+=s.vy*dt;s.vy-=dt*1.5f;s.life-=dt*1.4f;if(s.life<=0)s.on=false;}
  for(auto&r:rings)if(r.on){r.life-=dt;r.r+=dt*28;if(r.life<=0)r.on=false;}
  for(auto&b:bubbles)if(b.on){b.y-=b.vy*dt;b.x+=fsin(t*2.4f+b.phase)*dt*.7f;if(b.y<12)b.on=false;}
  pk().bubbleClock-=dt;if(pk().bubbleClock<0){pk().bubbleClock=rnd(.7f,1.6f);addBubble(rnd(layoutX(157),layoutX(160)),layoutY(282),rnd(.5f,1.25f),rnd(12,21));}
}

// A private pixel destination lets backdrop generation run during DMA without
// ever touching the framebuffer being transmitted. Creature drawing targets fb.
static inline void pkBlend(int x,int y,RGB8 c,int a){
  if((unsigned)x>=W||(unsigned)y>=H||a<=0)return;
  uint16_t &p=pk().pixels[y*W+x];
  int r=(p>>11)<<3,g=((p>>5)&63)<<2,b=(p&31)<<3;
  r+=((c.r-r)*a)>>8;g+=((c.g-g)*a)>>8;b+=((c.b-b)*a)>>8;
  p=pack565d(r,g,b,x,y);
}
static void pkGardenDot(int xq,int yq,RGB8 c,int a){
  int x=xq>>4,y=yq>>4,fx=xq&15,fy=yq&15;
  pkBlend(x,y,c,a*(16-fx)*(16-fy)>>8);pkBlend(x+1,y,c,a*fx*(16-fy)>>8);
  pkBlend(x,y+1,c,a*(16-fx)*fy>>8);pkBlend(x+1,y+1,c,a*fx*fy>>8);
}
static inline uint16_t pkGardenPhase(uint32_t k){return pk().baking?0:tph(k);}

// Integer pixel primitives. Everything entering these functions is Q4.
static void pkEllipse(int xq,int yq,int rxq,int ryq,RGB8 c,int alpha=255,
                      bool shaded=false,RGB8 hi={0,0,0},RGB8 dark={0,0,0},bool edge=false,RGB8 rim={0,0,0}){
  if(rxq<2||ryq<2||alpha<=0)return;
  int x0=clampi((xq-rxq-8)>>4,0,W-1),x1=clampi((xq+rxq+8)>>4,0,W-1);
  int y0=clampi((yq-ryq-8)>>4,0,H-1),y1=clampi((yq+ryq+8)>>4,0,H-1);
  int kx=(1<<22)/(rxq*rxq),ky=(1<<22)/(ryq*ryq),rm=rxq<ryq?rxq:ryq;
  const int nx=(100<<8)/rxq,ny=(95<<8)/ryq;
  for(int y=y0;y<=y1;y++){
    int dy=y*16+8-yq,dy2=(dy*dy*ky)>>14;
    for(int x=x0;x<=x1;x++){
      int dx=x*16+8-xq,d2=((dx*dx*kx)>>14)+dy2;
      int cov=clampi(((256-d2)*rm)/32+128,0,256);if(!cov)continue;
      RGB8 col=c;
      if(shaded){
        int light=clampi(160+((dx*nx)>>8)+((dy*ny)>>8)+(d2>>2),0,256);
        if(light<160){int q=(light*256)/160;col={(uint8_t)(hi.r+((c.r-hi.r)*q>>8)),(uint8_t)(hi.g+((c.g-hi.g)*q>>8)),(uint8_t)(hi.b+((c.b-hi.b)*q>>8))};}
        else{int q=(light-160)*256/96;col={(uint8_t)(c.r+((dark.r-c.r)*q>>8)),(uint8_t)(c.g+((dark.g-c.g)*q>>8)),(uint8_t)(c.b+((dark.b-c.b)*q>>8))};}
      }
      if(edge&&d2>210)col=rim;
      pkBlend(x,y,col,(cov*alpha)>>8);
    }
  }
}
static void pkLine(PPoint a,PPoint b,RGB8 color,int width,int alpha=255){
  int dx=b.x-a.x,dy=b.y-a.y,n=(abs(dx)>abs(dy)?abs(dx):abs(dy))/12+1;
  int x=a.x*256,y=a.y*256,sx=dx*256/n,sy=dy*256/n;
  for(int i=0;i<=n;i++,x+=sx,y+=sy){
    if(width>18)pkEllipse(x>>8,y>>8,width/2,width/2,color,alpha);
    else{int qx=x>>8,qy=y>>8,ix=qx>>4,iy=qy>>4,fx=qx&15,fy=qy&15,amp=alpha*(width<16?width:16)/16;
      pkBlend(ix,iy,color,amp*(16-fx)*(16-fy)>>8);pkBlend(ix+1,iy,color,amp*fx*(16-fy)>>8);
      pkBlend(ix,iy+1,color,amp*(16-fx)*fy>>8);pkBlend(ix+1,iy+1,color,amp*fx*fy>>8);
    }
  }
}
static void pkPolygon(const PPoint*pts,int n,RGB8 color,int alpha){
  int ymin=H*16,ymax=0;for(int i=0;i<n;i++){if(pts[i].y<ymin)ymin=pts[i].y;if(pts[i].y>ymax)ymax=pts[i].y;}
  int lo=clampi((ymin-8)>>4,0,H-1),hi=clampi((ymax+8)>>4,0,H-1);
  for(int y=lo;y<=hi;y++){
    int crossings[32],count=0,qy=y*16+8;
    for(int i=0,j=n-1;i<n;j=i++){
      const auto&a=pts[i];const auto&b=pts[j];if((a.y<=qy&&b.y>qy)||(b.y<=qy&&a.y>qy)){
        int x=a.x+(qy-a.y)*(b.x-a.x)/(b.y-a.y),k=count;
        if(count>=32)continue;while(k>0&&crossings[k-1]>x){crossings[k]=crossings[k-1];k--;}crossings[k]=x;count++;
      }
    }
    for(int i=0;i+1<count;i+=2){int a=crossings[i],b=crossings[i+1];int x0=clampi(a>>4,0,W-1),x1=clampi(b>>4,0,W-1);
      for(int x=x0;x<=x1;x++){int l=a>x*16?a:x*16,r=b<(x+1)*16?b:(x+1)*16;if(r>l)pkBlend(x,y,color,(r-l)*alpha>>4);}
    }
  }
}
struct PV {float x,y,z;PV(float X=0,float Y=0,float Z=0):x(X),y(Y),z(Z){}};
struct PProjected{int16_t x,y,depth;};
class PokemonPose {
  PokemonFish &f; PokemonWork &work; int nc=0,nv=0;
  int c,s,rc,rs,scale,tail,phase,originX,originY;
  PCommand* command(uint8_t type,int depth){
    if(nc>=PK_COMMANDS){pk().overflowCount++;return nullptr;}
    PCommand*q=&work.commands[nc++];*q=PCommand();q->type=type;q->depth=depth;q->alpha=255;return q;
  }
 public:
  PokemonPose(PokemonFish &fish,float angle=-1):f(fish),work(pk().work){
    uint16_t yaw=bam(angle<0?f.yaw:angle);c=icos(yaw);s=isin(yaw);phase=bam(f.phase);
    tail=isin(bam(f.tailYaw-f.yaw));scale=(int)(PK_SPEC[f.kind].scale*256);
    originX=pkQ(f.x);originY=pkQ(f.y);
    float pitch=0;
    if(f.kind==PK_CHINCHOU||f.kind==PK_MAGIKARP||f.kind==PK_GOLDEEN)pitch=clampf(fatan2(f.vy,fabsf(f.vx)+5)*fcos(f.yaw)*.75f+f.bank,-.42f,.42f);
    else if(f.kind==PK_HORSEA)pitch=fsin(f.phase*.5f)*.04f+f.bank*.5f;
    else if(f.kind==PK_TENTACOOL)pitch=clampf(-f.vx*.018f,-.1f,.1f);
    else if(f.kind==PK_STARYU)pitch=f.spin;
    rc=icos(bam(pitch));rs=isin(bam(pitch));
  }
  PProjected project(PV v){
    int x=pkQ(v.x),y=pkQ(v.y),z=pkQ(v.z),u=clampi(-x*256/240,0,256),u2=u*u>>8;
    z+=((tail*160)>>14)*u2/256+((isin((uint16_t)(phase+x*143))*18)>>14)*u2/256;
    y+=((isin((uint16_t)(phase+x*124))*4)>>14)*u/256;
    int xx=(x*c-z*s)>>14,depth=(x*s+z*c)>>14;
    int px=(xx*rc-y*rs)>>14,py=(xx*rs+y*rc)>>14;
    return {(int16_t)(originX+(px*scale>>8)),(int16_t)(originY+(py*scale>>8)),(int16_t)depth};
  }
  float surface(float nx,float nz){return nx*(s/16384.f)+nz*(c/16384.f);}
  void path(const PV*v,int n,uint32_t color,bool fill,uint32_t edge=0,int width=8,int alpha=255){
    if(n<2)return;if(nv+n>PK_VERTICES){pk().overflowCount++;return;}
    int start=nv,depth=0;for(int i=0;i<n;i++){auto p=project(v[i]);work.vertices[nv++]={p.x,p.y};depth+=p.depth;}
    auto*q=command(fill?1:2,depth/n);if(!q)return;q->start=start;q->n=n;q->color=pkColor(color);q->edge=pkColor(edge);q->bordered=edge!=0;q->width=width;q->alpha=alpha;
  }
  void shape(std::initializer_list<PV>v,uint32_t color,uint32_t edge=0,int width=8,int alpha=255){path(v.begin(),v.size(),color,true,edge,width,alpha);}
  void stroke(std::initializer_list<PV>v,uint32_t color,int width=10,int alpha=255){path(v.begin(),v.size(),color,false,0,width,alpha);}
  void stroke(const PV*v,int n,uint32_t color,int width=10,int alpha=255){path(v,n,color,false,0,width,alpha);}
  void solid(float x,float y,float z,float rx,float ry,float rz,uint32_t color,uint32_t edge=0,uint32_t hi=0,uint32_t dark=0,int alpha=255){
    auto p=project({x,y,z});auto*q=command(0,p.depth);if(!q)return;
    int a=pkQ(rx),b=pkQ(rz),ac=a*c/16384,bs=b*s/16384;
    // Integer square root (width is tiny) keeps the front view round.
    uint32_t num=ac*ac+bs*bs,res=0,bit=1u<<30;while(bit>num)bit>>=2;while(bit){if(num>=res+bit){num-=res+bit;res=(res>>1)+bit;}else res>>=1;bit>>=2;}
    q->x=p.x;q->y=p.y;q->rx=res*scale/256;q->ry=pkQ(ry)*scale/256;
    q->color=pkColor(color);q->edge=pkColor(edge);q->bordered=edge!=0;q->hi=pkColor(hi);q->dark=pkColor(dark);q->shaded=hi!=0;q->alpha=alpha;
  }
  void eye(float x,float y,float z,float r,float nx,float nz,bool cross=false){
    float visibility=surface(nx,nz);if(visibility<=.035f)return;
    auto p=project({x,y,z});int depth=p.depth+pkQ(r*.7f),rx=pkQ(r*clampf(visibility,.12f,1))*scale/256,ry=pkQ(r*1.08f)*scale/256,alpha=(int)(clampf(visibility*5,0,1)*255);
    auto*q=command(0,depth);if(!q)return;q->x=p.x;q->y=p.y;q->rx=rx;q->ry=ry;q->color=pkColor(cross?0xf9df79:0xf2f8dd);q->alpha=alpha;
    if(cross){
      auto*rq=command(3,depth);if(!rq)return;rq->x=p.x;rq->y=p.y;rq->rx=rx*7/10;rq->ry=ry*7/10;rq->alpha=alpha;rq->color=pkColor(0x564a46);
    }else{
      auto*rq=command(0,depth);if(!rq)return;*rq=*q;rq->x+=((c*pkQ(r*.12f))>>14);rq->rx=rx*45/100;rq->ry=ry*6/10;rq->color=pkColor(0x06202c);
      auto*h=command(0,depth);if(!h)return;*h=*q;h->x-=pkQ(r*.12f);h->y-=pkQ(r*.38f);h->rx=rx*17/100;h->ry=ry*17/100;h->color=pkColor(0xfffdf0);
    }
  }
  void light(float x,float y,float z,float radius,uint32_t color,int alpha){
    auto p=project({x,y,z});auto*q=command(4,p.depth);if(!q)return;q->x=p.x;q->y=p.y;q->rx=(int)(radius*scale/256);q->color=pkColor(color);q->alpha=alpha;
  }
  void draw(){
    if(nc>pk().maxCommands)pk().maxCommands=nc;if(nv>pk().maxVertices)pk().maxVertices=nv;
    for(int i=0;i<nc;i++){int j=i;while(j>0&&work.commands[work.order[j-1]].depth>work.commands[i].depth){work.order[j]=work.order[j-1];j--;}work.order[j]=i;}
    for(int k=0;k<nc;k++){auto&q=work.commands[work.order[k]];
      if(q.type==0)pkEllipse(q.x,q.y,q.rx,q.ry,q.color,q.alpha,q.shaded,q.hi,q.dark,q.bordered,q.edge);
      else if(q.type==1||q.type==2){auto*v=work.vertices+q.start;
        if(q.type==1)pkPolygon(v,q.n,q.color,q.alpha);
        if(q.type==2||q.bordered)for(int i=1;i<q.n;i++)pkLine(v[i-1],v[i],q.type==2?q.color:q.edge,q.width,q.alpha);
        if(q.type==1&&q.bordered)pkLine(v[q.n-1],v[0],q.edge,q.width,q.alpha);
      }else if(q.type==3){pkLine({q.x,(int16_t)(q.y-q.ry)},{q.x,(int16_t)(q.y+q.ry)},q.color,9,q.alpha);pkLine({(int16_t)(q.x-q.rx),q.y},{(int16_t)(q.x+q.rx),q.y},q.color,9,q.alpha);}
      else if(q.type==4)blitQ(q.x,q.y,kernFor(q.rx),q.alpha,q.color);
    }
  }
};

static void pkTail(PokemonPose&p,PokemonFish&f,float x,float length,float width,uint32_t color,uint32_t edge){
  PV v[18];float swing=fsin(f.phase)*width*.13f;
  for(int j=0;j<=8;j++){float u=j*.125f,z=fsin(f.phase-u*1.25f)*u*1.6f;v[j]={x-u*length,-width*u+swing*u*u,z};v[17-j]={x-u*length,width*u+swing*u*u,z};}
  p.path(v,18,color,true,edge,7,210);
  for(int j=-2;j<=2;j++)p.stroke({{x,0,0},{x-length,swing+j*width*.5f,fsin(f.phase-1.25f)*1.6f}},edge,6,180);
}
static void pkMagikarp(PokemonFish&f){
  PokemonPose p(f);float w=fsin(f.phase);
  blit(f.x,f.y,15,.10f,pkColor(0xff695e));pkTail(p,f,-8,8,5.2f,0xcecd9d,0xffecca);
  p.shape({{-7,-3},{-8,-7},{-5,-6},{-3,-10},{0,-7},{3,-9},{5,-4}},0xf7c958,0xfff1ae);
  p.shape({{-4,3},{-3,8},{1,6},{5,4}},0xdeae58,0xf1dca1);
  p.solid(0,0,0,9,6,4.8f,0xef714f,0xf8a47b,0xffbc85,0x9e3a42);
  for(int side=-1;side<=1;side+=2){
    for(int k=0;k<4;k++)for(int j=0;j<3;j++){
      float x=-5+k*2.7f,y=-2.8f+j*2.5f+(k%2)*.8f,z=side*4.85f*sqrtf(clampf(1-x*x/81-y*y/36,.05f,1));
      if(x<4&&fabsf(y)<5&&p.surface(x/12,side*.85f)>.12f)p.stroke({{x-.5f,y-.7f,z},{x+.55f,y,z},{x-.5f,y+.65f,z}},0xffc183,8,110);
    }
    float paddle=side*(6.7f+fsin(f.phase+side*.5f)*1.5f);
    p.shape({{-1,1,side*4.f},{-6,4+w,paddle},{-3,6+w,paddle},{2,2,side*4.f}},0xdfc49a,0xffdeb3,8,210);
    if(p.surface(.3f,side*.9f)>.08f)p.stroke({{4,-3.5f,side*3.6f},{3,-1,side*4.6f},{4,3,side*3.7f}},0xa04948,10);
    p.eye(6,-2,side*3.4f,2.35f,.64f,side*.77f);
    p.stroke({{8,3,side*1.8f},{7,6,side*3.5f},{4.8f,7+w*.4f,side*5.f}},0xffe397,10);
  }
  p.solid(9,1.4f,0,1.4f,2.2f+f.eat*.8f,1.85f,0xf6d196,0xffdca0);
  p.solid(9.8f,1.4f,0,.72f,1.35f+f.eat*.8f,1.1f,0x632f3f);p.draw();
}
static void pkGoldeen(PokemonFish&f){
  PokemonPose p(f);float swing=fsin(f.phase)*1.2f;blit(f.x,f.y,15,.10f,pkColor(0xf49d8b));
  pkTail(p,f,-7,12,7,0xe3eadb,0xdbf9e6);
  p.shape({{-14,-3+swing,.6f},{-19,-6+swing,.6f},{-17,-2+swing,.6f},{-18,1+swing,.6f},{-13,2+swing,.6f}},0xfa9765,0,8,190);
  p.shape({{-5,-2},{-6,-8},{-1,-5},{2,-7},{4,-3}},0xeef4dc,0xc7ebe4,7,220);
  p.solid(0,0,0,8,5,3.85f,0xd5e5de,0xe2ead7,0xf8f3cc,0x7499a7);
  for(int side=-1;side<=1;side+=2){
    if(p.surface(.5f,side*.8f)>0)p.solid(4.4f,-1.7f,side*2.85f,3.2f,2.9f,.65f,0xef805b);
    p.shape({{0,1,side*3.5f},{-6,5+fsin(f.phase)*1.6f,side*6.f},{-2,6.7f,side*6.f},{3,3,side*3.f}},0xf8f1d5,0xe1f8e8,7,185);
    p.eye(6,-1.2f,side*2.5f,1.6f,.67f,side*.74f);
  }
  p.shape({{-5,-4,1},{-1,-5.1f,1},{2,-3.4f,1},{-1,-1,3.4f}},0xef9461);
  p.shape({{5,-4,-1.2f},{10,-10,0},{8.5f,-3,1.1f}},0xf7e2ab,0xfff3cc,7);
  p.shape({{5,-4,1.2f},{10,-10,0},{8.5f,-3,-1.1f}},0xf7e2ab,0xfff3cc,7);
  p.solid(8,1.3f,0,1,.8f+f.eat,1.1f,0xbd725d);p.draw();
}
static void pkHorsea(PokemonFish&f){
  PokemonPose p(f);blit(f.x,f.y,13,.10f,pkColor(0x68def0));
  PV curl[26];curl[0]={1,4,0};
  for(int i=0;i<=24;i++){float u=i/24.f,a=u*PIF*2.05f,r=4.1f-u*3.1f;curl[i+1]={-2+fcos(a)*r,8+fsin(a)*r,fsin(f.phase*.65f+i*.145f)*.65f};}
  p.stroke(curl,26,0x57c4df,32);p.stroke(curl+1,25,0x9ee9e4,8);
  for(int side=-1;side<=1;side+=2)p.shape({{-4,-3,side*2.f},{-9,-1+fsin(f.phase*1.5f)*2,side*4.f},{-8,3,side*4.f},{-4,5,side*2.f}},0xa5dce1,0xc4f1db,8,160);
  p.solid(0,1,0,4.8f,7,4.2f,0x55beda,0x9bdfeb,0xa6ede7,0x246d9b);
  p.solid(2,2.5f,0,2.7f,4.6f,3.6f,0xe9d4a1);
  for(int j=0;j<3;j++){PV v[9];for(int k=0;k<=8;k++){float a=-1.2f+k*.3f;v[k]={2+2.65f*fcos(a),j*2+.4f,3.55f*fsin(a)};}p.stroke(v,9,0x9ea994,8);}
  p.solid(1,-6,0,5.9f,5.7f,4.6f,0x4eb1d4,0x8cdae3,0x8ee8ed,0x22698c);
  p.shape({{-3,-10},{-4,-14},{-1,-11},{0,-14},{2,-11},{4,-12},{4.5f,-9}},0x5ebada,0xa2e5e3,8);
  for(int j=0;j<8;j++){float a=j*TAUF/8,b=(j+1)*TAUF/8;p.shape({{5,-4.65f+fcos(a)*1.7f,fsin(a)*1.55f},{11,-4.65f+fcos(a)*1.7f,fsin(a)*1.55f},{11,-4.65f+fcos(b)*1.7f,fsin(b)*1.55f},{5,-4.65f+fcos(b)*1.7f,fsin(b)*1.55f}},j<4?0x70cadd:0x5db1cc);}
  p.solid(11,-4.65f,0,.8f,1.8f,1.65f,0x87d5df,0xb2ece6);p.solid(11.55f,-4.65f,0,.35f,1,1,0x21485e);
  for(int side=-1;side<=1;side+=2)p.eye(3,-7,side*4.05f,1.9f,.46f,side*.89f);p.draw();
}
static void pkChinchou(PokemonFish&f){
  PokemonPose p(f);pkTail(p,f,-7,4.5f,4,0x487bbc,0x80bcd2);blit(f.x,f.y,17,.13f,pkColor(0x5079e5));
  p.solid(0,0,0,8.6f,6.2f,5.7f,0x366091,0x719bbd,0x719bd0,0x183f6e);
  for(int side=-1;side<=1;side+=2){
    PV v[13];for(int j=0;j<=12;j++){float u=j/12.f;v[j]={-2+side*6*u,-3-9*fsin(u*PIF*.68f)+fsin(f.phase+u*2)*u,side*(3.3f+u*4.8f)-f.turnRate*u*u*.45f};}
    p.stroke(v,13,0x8ebddb,13);auto&a=v[12];
    p.light(a.x,a.y,a.z,12,0xffd269,(int)(115+24*fsin(tms*.002f+f.phase)));
    p.solid(a.x,a.y,a.z,2.2f,2.6f,2.2f,0xf9e87f);p.solid(a.x-.35f,a.y-.7f,a.z+.8f,.6f,1,.6f,0xfffed0);
    p.shape({{-1,3,side*4.f},{-5,7+fsin(f.phase)*1.2f,side*7.f},{0,6,side*7.f},{4,4,side*3.f}},0x4686b4,0x92c9d6,8);
    p.eye(5,-.5f,side*4.f,2.25f,.64f,side*.77f,true);
  }
  p.stroke({{7,2.6f,-1},{7.5f,3.1f,0},{7,2.6f,1}},0xb7d6d8,8);p.draw();
}
static void pkTentacool(PokemonFish&f){
  PokemonPose p(f,0);float pulse=.5f+.5f*fsin(f.phase),rx=8.5f+pulse*.7f,ry=7.8f-pulse*1.4f;
  for(int side=-1;side<=1;side+=2){PV v[19];for(int j=0;j<=18;j++){float u=j/18.f;v[j]={side*3.6f+fsin(tms*.002f-u*3.6f+side)*u*2.1f-f.vx*u*u*.28f,4+u*19,-2};}p.stroke(v,19,0x69b7c8,23,180);p.stroke(v,19,0xb2e1d4,8,190);}
  blit(f.x,f.y-2,16,.14f,pkColor(0x68def0));p.solid(0,0,0,rx,ry,rx,0x389faf,0x72ced0,0x74e8de,0x254d79);
  p.shape({{-5,2,1},{-3,6,1},{0,5,1},{3,6,1},{5,2,1}},0x5b8a99,0x94c3c8);
  for(int side=-1;side<=1;side+=2){
    p.solid(side*5.f,-4,2,2.7f,3.5f,2.7f,0xe86d89,0xf9a1a8,0xffbaae,0x963d73);
    p.light(side*5.f,-4,2,5,0xfe78b2,65);p.solid(side*5.f-.5f,-5.4f,3,.55f,1.1f,.55f,0xffd4c5);
    p.shape({{side*1.8f,2,3},{side*4.5f,1.8f,3},{side*3.5f,4.2f,3},{side*2.f,4,3}},0xddf1df);
    p.stroke({{side*3.1f,2.4f,4},{side*3.1f,3.7f,4}},0x154150,11);
  }
  p.solid(0,-1,3,1.9f,1.65f,1.9f,0xbf8fd8,0xc6afe7);p.draw();
}
static void pkWooper(PokemonFish&f){
  PokemonPose p(f,PIF*.35f+f.yaw*.3f);float w=fsin(f.phase),step=clampf(fabsf(f.vx)*.25f,0,1);
  p.shape({{-4,2,-2},{-11,1+w,-7},{-13,5+w,-8},{-8,7,-7},{-3,6,-2}},0x5faec7,0x9de3de,10);
  for(int side=-1;side<=1;side+=2)p.solid(1,7+side*w*step,side*3.8f,2.6f,1.9f,2.3f,0x68b9ca,0xaae5dd);
  p.solid(0,2,0,5,6.5f,6,0x68b8cd,0xa6dede,0x99dedc,0x43819c);
  for(int j=0;j<3;j++)p.stroke({{4.8f,2+j*1.6f,-2.3f+j*.22f},{5,2+j*1.6f,0},{4.8f,2+j*1.6f,2.3f-j*.22f}},0x3a718d,13);
  for(int side=-1;side<=1;side+=2){p.stroke({{1,-6,side*5.f},{1,-6,side*12.f}},0xc790bd,30);
    for(int j=0;j<3;j++){float z=side*(8+j*1.7f);p.stroke({{1,-6,z},{1,-9+j*.45f+fsin(tms*.0017f+j)*.2f,z}},0xcba0d0,20);p.stroke({{1,-6,z},{1,-3.5f-j*.3f,z}},0xba7bad,18);}
  }
  blit(f.x,f.y-4,13,.08f,pkColor(0x68def0));p.solid(1,-5,0,5.5f,5.7f,8,0x70c4d4,0xb1e6db,0xa5e2db,0x4a97b7);
  for(int side=-1;side<=1;side+=2)p.solid(5.9f,-6.1f,side*3.6f,.7f,.85f,.65f,0x123846);
  p.stroke({{6.1f,-3.8f,-2.5f},{6.4f,-2.9f,-1.2f},{6.5f,-2.6f,0},{6.4f,-2.9f,1.2f},{6.1f,-3.8f,2.5f}},0x215365,9);
  if(f.eat>0)p.solid(6.5f,-3,0,.6f,1.2f,1.1f,0x326077);p.draw();
}
static void pkStaryu(PokemonFish&f){
  PokemonPose p(f,0);PV v[10];for(int j=0;j<10;j++){float a=-PIF/2+j*PIF/5,r=j%2?4.15f:10.8f;v[j]={fcos(a)*r,fsin(a)*r,0};}
  blit(f.x,f.y,15,.1f,pkColor(0xffd269));p.path(v,10,0xc69550,true,0xe8c680,10);
  for(int j=0;j<5;j++){PV a=v[j*2],b=v[(j*2+1)%10],c=v[(j*2+9)%10];a.z=b.z=c.z=1;
    p.shape({{0,0,1},c,a},0xa77342,0,8,115);p.shape({{0,0,1},a,b},0xf3c172,0,8,115);
    p.stroke({{a.x*.58f,a.y*.58f,2},{a.x*.83f,a.y*.83f,2}},0xf9d895,8);
  }
  p.solid(0,0,3,4.4f,4.4f,4.4f,0xedc66c,0xffdea1);p.solid(0,0,4,3.2f,3.2f,3.2f,0x725348);
  p.solid(0,0,5,2.65f,2.65f,2.65f,0xe36280,0xffc0a1,0xffb5a7,0x973b6c);
  p.light(0,0,6,7,0xff695e,(int)(60+25*fsin(tms*.0016f)));p.solid(-.8f,-1.1f,7,.55f,.65f,.55f,0xffe1b5);p.draw();
}
static void pkDrawFish(PokemonFish&f){
  switch(f.kind){case PK_TENTACOOL:pkTentacool(f);break;case PK_CHINCHOU:pkChinchou(f);break;case PK_MAGIKARP:pkMagikarp(f);break;case PK_HORSEA:pkHorsea(f);break;case PK_GOLDEEN:pkGoldeen(f);break;case PK_WOOPER:pkWooper(f);break;case PK_STARYU:pkStaryu(f);break;}
  if(f.eat>0)blit(f.x+4,f.y-2,8,f.eat*.25f,pkColor(0xffd269));
}

// Mathematical habitat: hashed gravel, analytic rocks, sinusoidal ribbon leaves,
// pinnate ferns and recursively branching corals. No stored background bitmap.
static void pokemonBakeRow(int y,uint16_t *dst){
  static const RGB8 colors[4]={{18,61,68},{7,30,43},{3,14,28},{10,25,34}};
  const int y1=H/5,y2=H*7/10;
  int band=y<y1?0:y<y2?1:2,start=band==0?0:band==1?y1:y2,end=band==0?y1:band==1?y2:H;
  int u=(y-start)*256/(end-start);RGB8 a=colors[band],b=colors[band+1];
  for(int x=0;x<W;x++){
    int r=a.r+((b.r-a.r)*u>>8),g=a.g+((b.g-a.g)*u>>8),bl=a.b+((b.b-a.b)*u>>8);
    int d=(x-layoutXi(54))*(x-layoutXi(54))+y*y;if(d<11025){int q=(11025-d)/550;r+=q/4;g+=q;bl+=q;}
    if(y>layoutYi(283)&&y>=(int)pkGround(x)){
      int fade=clampi((H-y)*4,0,148);r=11+(21*fade>>8);g=21+(38*fade>>8);bl=30+(26*fade>>8);
      uint32_t h=hh(x,y);if((h&7)<2){int k=(h>>8)&23;r+=k;g+=k;bl+=k*3/4;}
    }
    static const int rock[4][4]={{108,285,25,13},{125,277,16,13},{11,292,16,10},{155,294,17,11}};
    if(y>layoutYi(260))for(int i=0;i<4;i++){
      int dx=x-layoutXi(rock[i][0]),dy=y-layoutYi(rock[i][1]),qx=dx*256/rock[i][2],qy=dy*256/rock[i][3],rad=(qx*qx+qy*qy)>>8;
      if(rad<244+((hh(x/4,y/3)>>24)&15)){
        int lum=clampi(80+qx/3+qy/2+rad/3,0,256);r=54-(43*lum>>8);g=75-(54*lum>>8);bl=80-(47*lum>>8);
        if((hh(x,y)&63)==0){r+=12;g+=17;bl+=12;}
      }
    }
    // Fixed broad shafts are baked once. Only the surface and vegetation move.
    if(y<layoutYi(225)&&y>8)for(int k=0;k<4;k++){int ry=landscape?9+(y-9)*269/121:y;int cx=layoutXi(13+k*32+ry/5);if(x>cx&&x<cx+layoutXi(5+k+ry/12)){int q=(225-ry)/32;g+=q;bl+=q;}}
    dst[y*W+x]=pack565d(r,g,bl,x,y);
  }
}
static PPoint pkPlantPoint(int x,int base,int height,int u,int ph){
  int bend=(isin((uint16_t)(u*126+ph+pkGardenPhase(K16(.00062))))*u*u/256)>>14;
  return {(int16_t)(x*16+bend/4+((isin((uint16_t)(ph+u*82))*u)>>16)),(int16_t)(base*16-height*u/16)};
}
static void pkFern(int x,int height,int ph,RGB8 col){
  x=layoutXi(x);height=(int)(height*depthScale);
  int base=(int)pkGround(x);PPoint prev=pkPlantPoint(x,base,height,0,ph);
  for(int j=1;j<=18;j++){
    int u=j*256/18;PPoint p=pkPlantPoint(x,base,height,u,ph);pkLine(prev,p,col,10,190);prev=p;
    if(j<3)continue;int len=(20-j)*10;
    for(int side=-1;side<=1;side+=2){PPoint end={(int16_t)(p.x+side*len),(int16_t)(p.y+len/3)};pkLine(p,end,col,9,175);
      for(int k=1;k<4;k++){PPoint m={(int16_t)(p.x+(end.x-p.x)*k/4),(int16_t)(p.y+(end.y-p.y)*k/4)};PPoint e={(int16_t)(m.x+side*(4-k)*7),(int16_t)(m.y-24)};pkLine(m,e,col,7,145);}
    }
  }
}
static void pkRibbon(int x,int height,int width,int ph,RGB8 col){
  x=layoutXi(x);height=(int)(height*depthScale);
  PPoint v[22];int base=(int)pkGround(x);
  for(int j=0;j<=10;j++){int u=j*256/10;PPoint p=pkPlantPoint(x,base,height,u,ph);int w=width*(256-u)/256;v[j]={(int16_t)(p.x-w),p.y};v[21-j]={(int16_t)(p.x+w),p.y};}
  pkPolygon(v,22,col,135);for(int j=1;j<=10;j++)pkLine(v[j-1],v[j],col,7,125);
}
static void pkCoral(int x,int y,uint16_t angle,int len,int level,int seed,RGB8 color){
  PPoint a={(int16_t)x,(int16_t)y},b={(int16_t)(x+((icos(angle)*len)>>14)),(int16_t)(y+((isin(angle)*len)>>14))};
  pkLine(a,b,color,level*3+8,180);
  if(level){int spread=3400+(hh(seed,level)&2047);pkCoral(b.x,b.y,angle-spread,len*67/100,level-1,seed+1,color);pkCoral(b.x,b.y,angle+spread,len*63/100,level-1,seed+7,color);}
  else pkGardenDot(b.x,b.y,color,90+((isin((uint16_t)(seed*1747+pkGardenPhase(K16(.001))))+16384)>>9));
}
static void pokemonGarden(bool front){
  if(!front){
    pkFern(14,150,5000,pkColor(0x397e73));pkFern(7,101,27500,pkColor(0x2b777c));pkFern(166,123,12000,pkColor(0x357e7e));
    for(int i=0;i<4;i++)pkRibbon(143+i*5,119+i*15,16+i*4,7000+i*11300,pkColor(0x288073));
    pkCoral(layoutXi(146)*16,layoutYi(283)*16,49152,(int)(26*16*(landscape?.7f:1.f)),4,17,pkColor(0x986f9a));pkCoral(layoutXi(25)*16,layoutYi(286)*16,49152,(int)(20*16*(landscape?.7f:1.f)),4,63,pkColor(0x716e9d));
  }else{
    for(int i=0;i<4;i++)pkRibbon(4+i*7,61+i*13,15+i*3,2500+i*6100,pkColor(0x358c70));
    for(int i=0;i<36;i++){
      int x=hh(i,991)%W,height=5+(hh(i,54)%16);PPoint prev={(int16_t)(x*16),(int16_t)(pkQ(pkGround(x))+96)};
      for(int j=1;j<=4;j++){PPoint p={(int16_t)(x*16+((isin((uint16_t)(j*4800+i*7300+pkGardenPhase(K16(.0009))))*j*j)>>14)),(int16_t)(pkQ(pkGround(x))+96-height*j*4)};pkLine(prev,p,pkColor(0x5a9e85),8,150);prev=p;}
    }
    static const int A[3][4]={{32,292,13,7},{142,299,16,8},{99,304,11,5}};
    for(int i=0;i<3;i++){
      RGB8 c=pkColor(i==0?0xad93e4:i==1?0x74d5b0:0xd483ac);
      for(int j=0;j<A[i][2];j++){
        int a=j*65536/A[i][2],r=A[i][3]*16+((isin((uint16_t)(pkGardenPhase(K16(.0017))+j*3000))*12)>>14);
        PPoint b={(int16_t)(layoutXi(A[i][0])*16),(int16_t)(layoutYi(A[i][1])*16+24)},tip={(int16_t)(b.x+((icos(a)*r)>>14)),(int16_t)(b.y-60+((isin(a)*r*3)>>16))};
        pkLine(b,tip,c,9,130);addDot(tip.x,tip.y,c,125);
      }
    }
  }
}
static void renderPokemon(){
  for(int i=0;i<80;i++){auto&m=motes[i];int xq=pkQ(m.x),yq=pkQ(m.y),energy=0;
    for(const auto&f:pk().fish){int dx=f.qx-xq,dy=f.qy-yq;if(abs(dx)>10*16||abs(dy)>10*16)continue;if(dx*dx+dy*dy<95*256){energy=120;break;}}
    addPx(xq>>4,yq>>4,112,198,187,25+(int)(m.z*45)+energy);
  }
  // Sort whole animals by vertical habitat. Features inside each animal sort by
  // projected depth, so eyes, fins and antennae pass behind rounded bodies.
  uint8_t order[PK_COUNT];for(int i=0;i<PK_COUNT;i++){int j=i;while(j>0&&pk().fish[order[j-1]].y>pk().fish[i].y){order[j]=order[j-1];j--;}order[j]=i;}
  for(int i=0;i<PK_COUNT;i++)pkDrawFish(pk().fish[order[i]]);
  for(auto&q:food)if(q.on){blitQ(q.qx,q.qy,7,95,pkColor(0xffd269));pkEllipse(q.qx,q.qy,10,7,pkColor(0xf8cf8d));}
  for(auto&s:sparks)if(s.on)blit(s.x,s.y,3,s.life*.5f,s.c);
}
static void pokemonFront(){
  pokemonGarden(true);
  for(auto&b:bubbles)if(b.on){int cx=pkQ(b.x),cy=pkQ(b.y),r=pkQ(b.r);for(int k=0;k<8;k++)addDot(cx+((icos(k*8192)*r)>>14),cy+((isin(k*8192)*r)>>14),pkColor(0x95cace),65);addDot(cx-5,cy-5,pkColor(0xcff2df),110);}
  for(int x=0;x<W;x++){int y=8*16+((isin((uint16_t)(x*1043+tph(K16(.0011))))*10)>>14)+((isin((uint16_t)(x*2400-tph(K16(.0007))))*6)>>14);addDot(x*16,y,pkColor(0xa3dbc7),95);addDot(x*16,y+28,pkColor(0x418f87),50);}
  for(auto&r:rings)if(r.on)for(int k=0;k<36;k++){int a=k*65536/36;addDot(pkQ(r.x)+((icos(a)*pkQ(r.r))>>14),13*16+((isin(a)*pkQ(r.r*.17f))>>14),pkColor(0xdbebb4),(int)(r.life*110));}
}

// Back plants are still generated from their equations on the device. Cache
// their pixels with the existing background once, then sway rows in O(H).
// Foreground leaves, anemones, creatures, food and particles remain animated.
static void pokemonPrepareBase(){
  if(!bgS||bakeM>=0)return;
  for(int y=0;y<H;y++)pokemonBakeRow(y,bgS);
  pk().pixels=bgS;pk().baking=true;pokemonGarden(false);pk().baking=false;pk().pixels=fb;
  bakeM=0;bakeNight=0;
}
static void pokemonRenderBase(){
  if(bgS){if(!basePrepared)pokemonPrepareBase();}
  else{
    for(int y=0;y<H;y++)pokemonBakeRow(y,fb);
    pk().pixels=fb;pk().baking=true;pokemonGarden(false);pk().baking=false;
  }
  basePrepared=false;
  for(int y=0;y<H;y++){
    int ry=landscape?9+(y-9)*269/121:y;
    int amplitude=clampi(270-ry,0,190)*3;
    int shift=(isin((uint16_t)(tph(K16(.00062))+y*127))*amplitude)>>22;
    uint16_t *out=fb+y*W;const uint16_t*src=(bgS?bgS:fb)+y*W;
    if(shift>0){uint16_t edge=src[0];memmove(out+shift,src,(W-shift)*2);for(int x=0;x<shift;x++)out[x]=edge;}
    else if(shift<0){int k=-shift;uint16_t edge=src[W-1];memmove(out,src+k,(W-k)*2);for(int x=W-k;x<W;x++)out[x]=edge;}
    else if(bgS)memcpy(out,src,W*2);
  }
}
