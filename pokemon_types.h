#pragma once
// Included inside namespace gt. All artwork is runtime mathematical geometry.
#ifndef GT_POKEMON_COPIES
#define GT_POKEMON_COPIES 1
#endif
static_assert(GT_POKEMON_COPIES>=1 && GT_POKEMON_COPIES<=3,"Use one, two or three of each Pokemon");
enum PokemonKind { PK_TENTACOOL, PK_CHINCHOU, PK_MAGIKARP, PK_HORSEA, PK_GOLDEEN, PK_WOOPER, PK_STARYU, PK_KINDS };
static const int PK_COUNT=PK_KINDS*GT_POKEMON_COPIES;
struct PokemonFish {
  float x,y,vx,vy,yaw,yawTarget,tailYaw,turnRate,bank,phase,clock,tx,ty,cooldown,eat,spin,speedScale;
  uint16_t meals; int16_t qx,qy; uint8_t kind; int8_t target;
};
struct PPoint { int16_t x,y; }; // screen coordinates, Q4
struct PCommand {
  int16_t depth,x,y,rx,ry; uint16_t start;
  RGB8 color,hi,dark,edge;
  uint8_t type,n,width,alpha,shaded,bordered;
};
static const int PK_COMMANDS=96, PK_VERTICES=512;
struct PokemonWork { PCommand commands[PK_COMMANDS]; PPoint vertices[PK_VERTICES]; uint8_t order[PK_COMMANDS]; };
struct PokemonState {
  PokemonFish fish[PK_COUNT]; PokemonWork work;
  uint16_t *pixels; bool baking;
  float bubbleClock,feedCooldown;
  uint32_t overflowCount; uint16_t maxCommands,maxVertices;
};
