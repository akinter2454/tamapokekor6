#pragma once
#include <stdint.h>
#include "dex.h"

// The gym ladders: 8 leaders, the Elite 4 and the Champion for each region,
// with their real teams and levels -- Kanto from FireRed/LeafGreen, Johto from
// Gold/Silver/Crystal, Hoenn from Ruby/Sapphire/Emerald.
//
// HAND-AUTHORED -- not generated. The levels are the real ones and they happen
// to fit this game's curve almost exactly: level is age at 1/hour, a pet retires
// at 73 after three days and caps at 100, so Brock at 12-14 is an afternoon and
// Lance at 54-60 is a well-raised creature. Nothing was rescaled.
//
// There is deliberately no gating. A leader always brings its whole team and you
// bring whoever you have, so attrition is the difficulty: one strong creature
// can sweep Brock but will not survive five of Lance's in a row.

// FOUR ROSTER SLOTS ARE DELIBERATE SUBSTITUTIONS, not transcription errors.
// SpriteCollab has no art for these, so as written they put a bare dex NUMBER on
// screen where a leader's creature should be -- and three of them LEAD, which is
// the first thing you see in the fight:
//
//   ELESA   ZEBSTRIKA  523 -> GALVANTULA 596   (Bug/Electric, still fast)
//   MARLON  CARRACOSTA 565 -> SEISMITOAD 537   (bulky Water)
//   MARSHAL THROH      538 -> HARIYAMA   297   (bulky pure Fighting; see below)
//   MALVA   PYROAR     668 -> DELPHOX    655   (fast special Fire, Kalos)
//
// Each stand-in keeps the leader's specialty type, sits near the original's
// base-stat total and comes from the same region as the ladder, so the fight
// still reads as that region's. Levels are unchanged.
//
// MARSHAL IS THE ONE EXCEPTION, and deliberately. Unova has no pure Fighting
// left once Conkeldurr, Sawk and Mienshao are already on his team and Throh has
// no art: what remains is Gurdurr (BST 405, and Marshal already carries its own
// evolution), the 580-BST musketeer legendaries, or Scrafty -- which GRIMSLEY
// already leads with, so the Fighting specialist would inherit the Dark
// specialist's signature. Hariyama is Hoenn but is what Throh actually was: a
// slow, enormously bulky pure-Fighting grappler, HP 144 to Throh's 120.
//
// This REVERSES the earlier decision to keep art-less mons and let them draw as
// numbers. That was defensible while it affected mid-team slots; a gym leader
// leading with a number is not. roster_test now fails if any team contains a
// species from noart.h, so a future generation cannot reintroduce this quietly.
// It does mean Unova no longer matches B2W2 exactly -- and verify_rosters.py
// cannot catch that, since there is no Gen 5 decomp to diff against.

#define TRAINER_TEAM_MAX 6
// The ladders are levelled to this game's curve, where 100 is the ceiling.
#define MAX_TRAINER_LEVEL 100

// v3.61 catalog keeps National Dex 1..809 at their original IDs and places
// later base species at natdex+18, leaving 810..827 for the fixed Alola forms.
// Trainer rosters can therefore stay readable in National-Dex numbers.
#define TP_NATDEX(n) ((uint16_t)(((n) <= 809) ? (n) : ((n) + 18)))

struct TrainerMon {
  uint16_t dex;      // NOT uint8_t: Hoenn runs to 386
  uint8_t level;
};

struct Trainer {
  const char *name;
  const char *place;   // gym town, or the Elite 4 room
  uint8_t type;        // the type they specialise in, for the UI accent
  uint8_t count;
  TrainerMon team[TRAINER_TEAM_MAX];
};

// index 0-7 are the badges, 8-11 the Elite 4, 12 the Champion
#define TRAINER_COUNT 13
#define TRAINER_GYMS 8
#define TRAINER_ELITE4 4

static const Trainer TRAINERS_KANTO[TRAINER_COUNT] = {
  { "웅",    "PEWTER",    T_ROCK,     2, { {74,12},{95,14} } },
  { "이슬",    "CERULEAN",  T_WATER,    2, { {120,18},{121,21} } },
  { "마티스", "VERMILION", T_ELECTRIC, 3, { {100,21},{25,18},{26,24} } },
  { "민화",    "CELADON",   T_GRASS,    3, { {71,29},{114,24},{45,29} } },
  { "독수",     "FUCHSIA",   T_POISON,   4, { {109,37},{89,39},{109,37},{110,43} } },
  { "초련",  "SAFFRON",   T_PSYCHIC,  4, { {64,38},{122,37},{49,38},{65,43} } },
  { "강연",   "CINNABAR",  T_FIRE,     4, { {58,42},{77,40},{78,42},{59,47} } },
  { "비주기", "VIRIDIAN",  T_GROUND,   5, { {111,45},{51,42},{31,44},{34,45},{112,50} } },
  { "칸나",  "ELITE 4",   T_ICE,      5, { {87,52},{91,51},{80,52},{124,54},{131,54} } },
  { "시바",    "ELITE 4",   T_FIGHTING, 5, { {95,51},{107,53},{106,53},{95,54},{68,56} } },
  { "국화",   "ELITE 4",   T_GHOST,    5, { {94,54},{42,54},{93,53},{24,56},{94,58} } },
  { "목호",    "ELITE 4",   T_DRAGON,   5, { {130,56},{148,54},{148,54},{142,58},{149,60} } },
  { "라이벌",    "CHAMPION",  T_NORMAL,   6, { {18,61},{65,59},{112,61},{59,63},{103,61},{9,65} } },
};


// Johto: Gold/Silver/Crystal. The levels run lower than Kanto's early on and
// the Elite 4 sits at 40-50, which lands a Johto run comfortably inside a
// three-day life the same way Kanto's does.
static const Trainer TRAINERS_JOHTO[TRAINER_COUNT] = {
  { "비상",  "VIOLET",     T_FLYING,   2, { {16,7},{17,9} } },
  { "호일",    "AZALEA",     T_BUG,      3, { {11,14},{14,14},{123,16} } },
  { "꼭두",  "GOLDENROD",  T_NORMAL,   2, { {35,18},{241,20} } },
  { "유빈",    "ECRUTEAK",   T_GHOST,    4, { {92,21},{93,21},{94,25},{93,23} } },
  { "사도",    "CIANWOOD",   T_FIGHTING, 2, { {57,27},{62,30} } },
  { "규리",  "OLIVINE",    T_STEEL,    3, { {81,30},{81,30},{208,35} } },
  { "류옹",    "MAHOGANY",   T_ICE,      3, { {86,27},{87,29},{221,31} } },
  { "이향",    "BLACKTHORN", T_DRAGON,   4, { {148,37},{148,37},{148,37},{230,40} } },
  { "일목",     "ELITE 4",    T_PSYCHIC,  5, { {178,40},{124,41},{103,41},{80,41},{178,42} } },
  { "독수",     "ELITE 4",    T_POISON,   5, { {168,40},{49,41},{205,43},{89,42},{169,44} } },
  { "시바",    "ELITE 4",    T_FIGHTING, 5, { {237,42},{106,42},{107,42},{95,43},{68,46} } },
  { "카렌",    "ELITE 4",    T_DARK,     5, { {197,42},{45,42},{94,45},{198,44},{229,47} } },
  { "목호",    "CHAMPION",   T_DRAGON,   6, { {130,44},{149,47},{149,47},{142,46},{6,46},{149,50} } },
};

// Hoenn: EMERALD, consistently. That choice follows from Juan being the eighth
// leader -- in Ruby/Sapphire that seat is Wallace's and Steven is the champion,
// while in Emerald Juan takes the gym and Wallace the title. Mixing the two
// would have given a ladder that exists in neither game. Emerald's Steven is a
// post-game rematch at level 77 and is deliberately not here.
static const Trainer TRAINERS_HOENN[TRAINER_COUNT] = {
  { "원규",  "RUSTBORO",   T_ROCK,     3, { {74,12},{74,12},{299,15} } },
  { "철구",   "DEWFORD",    T_FIGHTING, 3, { {66,16},{307,16},{296,19} } },
  { "암페어",  "MAUVILLE",   T_ELECTRIC, 4, { {100,20},{309,20},{82,22},{310,24} } },
  { "민지", "LAVARIDGE",  T_FIRE,     4, { {322,24},{218,24},{323,26},{324,29} } },
  { "종길",   "PETALBURG",  T_NORMAL,   4, { {327,27},{288,27},{264,29},{289,31} } },
  { "은송",   "FORTREE",    T_FLYING,   5, { {333,29},{357,29},{279,30},{227,31},{334,33} } },
  { "풍",     "MOSSDEEP",   T_PSYCHIC,  4, { {344,41},{178,41},{337,42},{338,42} } },
  { "아단",     "SOOTOPOLIS", T_WATER,    5, { {370,41},{340,41},{364,43},{342,43},{230,46} } },
  { "혁진",   "ELITE 4",    T_DARK,     5, { {262,46},{275,48},{332,46},{342,48},{359,49} } },
  { "회연",   "ELITE 4",    T_GHOST,    5, { {356,48},{354,49},{302,50},{354,49},{356,51} } },
  { "미혜",   "ELITE 4",    T_ICE,      5, { {364,50},{362,50},{364,52},{362,52},{365,53} } },
  { "권수",    "ELITE 4",    T_DRAGON,   5, { {372,52},{334,54},{230,53},{330,53},{373,55} } },
  { "윤진",  "CHAMPION",   T_WATER,    6, { {321,57},{73,55},{272,56},{340,56},{130,56},{350,58} } },
};

// One ladder per region, in the same order as REGIONS in dex.h.
struct TrainerSet {
  const Trainer *list;
  const char *region;
  uint8_t regionId;   // index in dex.h REGIONS; gym ladders skip Hisui
};
// SINNOH -- PLATINUM, and the order matters: Fantina is the THIRD gym here,
// where Diamond/Pearl put her fifth. Taken from pret/pokeplatinum's own
// res/trainers/data/*.json (rematches are separate files there, so unlike
// Crystal and Emerald there is no "first party" ambiguity to get wrong).
//
// The level ramp is what pins the order: 14/22/26/32/37/41/44/50 is monotonic
// only with Fantina third, and roster_test fails a leader 8+ levels below the
// previous -- the D/P order trips it (Fantina 26 straight after Wake 37).
static const Trainer TRAINERS_SINNOH[TRAINER_COUNT] = {
  { "강석",   "OREBURGH",  T_ROCK,     3, { {74,12},{95,12},{408,14} } },
  { "유채","ETERNA",    T_GRASS,    3, { {387,20},{421,20},{407,22} } },
  { "멜리사", "HEARTHOME", T_GHOST,    3, { {355,24},{93,24},{429,26} } },
  { "자두", "VEILSTONE", T_FIGHTING, 3, { {307,28},{67,29},{448,32} } },
  { "맥실러",    "PASTORIA",  T_WATER,    3, { {130,33},{195,34},{419,37} } },
  { "동관",   "CANALAVE",  T_STEEL,    3, { {82,37},{208,38},{411,41} } },
  { "무청", "SNOWPOINT", T_ICE,      4, { {215,40},{221,40},{460,42},{478,44} } },
  { "전진", "SUNYSHORE", T_ELECTRIC, 4, { {135,46},{26,46},{405,48},{466,50} } },
  { "충호",   "ELITE 4",   T_BUG,      5, { {469,49},{212,49},{416,50},{214,51},{452,53} } },
  { "들국화",  "ELITE 4",   T_GROUND,   5, { {340,50},{472,53},{450,52},{76,52},{464,55} } },
  { "대엽",   "ELITE 4",   T_FIRE,     5, { {229,52},{136,55},{78,53},{392,55},{467,57} } },
  { "오엽",  "ELITE 4",   T_PSYCHIC,  5, { {122,53},{196,55},{437,54},{65,56},{475,59} } },
  { "난천", "CHAMPION",  T_DRAGON,   6, { {442,58},{407,58},{468,60},{448,60},{350,58},{445,62} } },
};

// UNOVA -- BLACK 2 / WHITE 2, and *** NOT VERIFIED AGAINST A DISASSEMBLY ***.
//
// THIS ONE IS DIFFERENT FROM THE OTHER FOUR. Kanto, Johto, Hoenn and Sinnoh are
// checked by tools/verify_rosters.py against pokered/pokecrystal/pokeemerald/
// pokeplatinum -- the games' own tables. pret has no Gen 5 disassembly (their
// DS work stops at Platinum) and no machine-readable substitute was found, so
// this ladder is written from knowledge. That is EXACTLY how Johto and Hoenn
// were first written, and verify_rosters.py later found ten errors in them,
// including two trainers carrying the wrong game's team entirely. Treat every
// level here as approximate until a source exists.
//
// B2W2 rather than Black/White because BW has two AMBIGUOUS leaders: the
// Striaton trio (which of Cilan/Chili/Cress you face depends on your starter)
// and Drayden-or-Iris (version). B2W2 has neither, and a single fixed ladder
// cannot represent a choice.
//
// Zebstrika (523), Throh (538) and Carracosta (565) have no sprite upstream and
// will draw as dex numbers. Kept faithful rather than substituted -- see
// noart.h; they are barred from the EGG POOL, not from existing.
static const Trainer TRAINERS_UNOVA[TRAINER_COUNT] = {
  { "체렌",  "ASPERTIA",  T_NORMAL,   2, { {504,11},{506,13} } },
  { "보미카",   "VIRBANK",   T_POISON,   2, { {109,16},{544,18} } },
  { "아티",   "CASTELIA",  T_BUG,      3, { {541,21},{557,21},{542,23} } },
  { "카밀레",   "NIMBASA",   T_ELECTRIC, 3, { {587,25},{180,25},{596,27} } },  // 523 ZEBSTRIKA: no art
  { "야콘",    "DRIFTVEIL", T_GROUND,   3, { {552,29},{28,29},{530,31} } },
  { "풍란",   "MISTRALTON",T_FLYING,   3, { {528,33},{227,33},{581,35} } },
  { "사간", "OPELUCID",  T_DRAGON,   3, { {621,43},{330,43},{612,46} } },
  { "시즈",  "HUMILAU",   T_WATER,    3, { {537,49},{321,49},{593,51} } },  // 565 CARRACOSTA: no art
  { "망초","ELITE 4",   T_GHOST,    4, { {563,56},{426,56},{623,56},{609,58} } },
  { "블래리","ELITE 4",   T_DARK,     4, { {510,56},{560,56},{553,56},{625,58} } },
  { "카틀레야", "ELITE 4",   T_PSYCHIC,  4, { {518,56},{561,56},{579,56},{576,58} } },
  { "연무", "ELITE 4",   T_FIGHTING, 4, { {297,56},{539,56},{534,56},{620,58} } },  // 538 THROH: no art
  { "아이리스",    "CHAMPION",  T_DRAGON,   6, { {635,59},{621,57},{306,57},{567,57},{131,57},{612,59} } },
};

// KALOS -- X / Y, and *** NOT VERIFIED AGAINST A DISASSEMBLY ***, for the same
// reason Unova is not: there is no Gen 6 decomp. pret's work stops at the DS
// generation, and Gen 6 is 3DS. Checked by hand against the games as recalled,
// which is precisely the standard that produced ten errors in Johto and Hoenn.
//
// Kalos is the LAST generation whose sprites are 100% complete upstream, which
// is why it was chosen over Alola/Galar/Paldea -- see check_sprites.py.
static const Trainer TRAINERS_KALOS[TRAINER_COUNT] = {
  { "비올라",    "SANTALUNE", T_BUG,      2, { {283,10},{666,12} } },
  { "자크로",    "CYLLAGE",   T_ROCK,     2, { {698,25},{696,25} } },
  { "코르니",  "SHALOUR",   T_FIGHTING, 3, { {619,29},{67,28},{701,32} } },
  { "후쿠지",    "COUMARINE", T_GRASS,    3, { {189,30},{70,31},{673,34} } },
  { "시트론",  "LUMIOSE",   T_ELECTRIC, 3, { {587,35},{82,35},{695,37} } },
  { "마슈",  "LAVERRE",   T_FAIRY,    3, { {303,38},{122,39},{700,42} } },
  { "고지카",  "ANISTAR",   T_PSYCHIC,  3, { {561,44},{199,45},{678,48} } },
  { "우르프",  "SNOWBELLE", T_ICE,      3, { {460,56},{615,55},{713,59} } },
  { "파키라",    "ELITE 4",   T_FIRE,     4, { {655,63},{324,63},{609,63},{663,65} } },  // 668 PYROAR: no art
  { "즈미",  "ELITE 4",   T_WATER,    4, { {693,63},{130,63},{121,63},{689,65} } },
  { "간피", "ELITE 4",   T_STEEL,    4, { {707,63},{476,63},{212,63},{681,65} } },
  { "드라세나",   "ELITE 4",   T_DRAGON,   4, { {691,63},{621,63},{334,63},{715,65} } },
  { "카르네",  "CHAMPION",  T_FAIRY,    6, { {701,64},{697,65},{699,65},{711,65},{706,66},{282,68} } },
};

// ALOLA -- SUN / MOON, and *** NOT VERIFIED AGAINST A DISASSEMBLY ***: there is
// no Gen 7 decomp either.
//
// ALOLA HAS NO GYMS. It has seven trial captains and four island kahunas, so the
// eight "gym" slots are six captains plus the two kahunas the Elite Four does
// not need, in ISLAND ORDER -- Melemele, Akala, Ula'ula, Poni -- which is also
// the game's own difficulty order. That leaves Hala, Olivia, Acerola and Kahili
// free to be the real Sun/Moon Elite Four, with no trainer appearing twice.
//
// Alola has no badge art anywhere (SteGriff stops at Unova; the Kalos sheet is
// one artist's). BADGE_REGIONS stays 6 while GYM_REGIONS becomes 7, and
// badgeArtFor() returns nullptr for Alola so the win screen and player card draw
// the plain type-coloured medal instead of borrowing another region's badges.
static const Trainer TRAINERS_ALOLA[TRAINER_COUNT] = {
  { "일리마",    "MELEMELE",  T_NORMAL,   2, { {734,10},{731,11} } },
  { "수련",     "AKALA",     T_WATER,    2, { {746,18},{752,20} } },
  { "키아웨",    "AKALA",     T_FIRE,     2, { {757,22},{776,24} } },
  { "마오",   "AKALA",     T_GRASS,    3, { {753,24},{762,24},{754,26} } },
  { "마마네","ULAULA",    T_ELECTRIC, 3, { {737,30},{777,31},{738,33} } },
  { "말리화",     "PONI",      T_FAIRY,    3, { {743,38},{210,38},{764,40} } },
  { "나누",     "ULAULA",    T_DARK,     3, { {302,42},{552,42},{53,44} } },
  { "하푸우",     "PONI",      T_GROUND,   4, { {623,47},{423,47},{330,49},{750,51} } },
  { "할라",     "ELITE 4",   T_FIGHTING, 4, { {297,54},{740,54},{57,54},{760,56} } },
  { "라이치",   "ELITE 4",   T_ROCK,     4, { {348,54},{346,54},{526,54},{745,56} } },
  { "아세로라",  "ELITE 4",   T_GHOST,    4, { {426,54},{770,54},{478,54},{781,56} } },
  { "카일리",   "ELITE 4",   T_FLYING,   4, { {628,54},{701,54},{733,54},{630,56} } },
  { "쿠쿠이",    "CHAMPION",  T_NORMAL,   6, { {745,57},{38,56},{628,56},{462,56},{143,56},{727,58} } },
};


// GALAR -- Sword gym order. Dynamax/Gigantamax are intentionally ignored in
// TamaPoke: these are ordinary single battles using the leaders' species/levels.
// Galar has no traditional Elite Four, so slots 8..11 represent the Champion
// Cup/endgame sequence (Marnie, Bede, Hop, Rose) before Champion Leon.
static const Trainer TRAINERS_GALAR[TRAINER_COUNT] = {
  { "아킬",   "TURFFIELD",  T_GRASS,    2, { {TP_NATDEX(829),19},{TP_NATDEX(830),20} } },
  { "야청",  "HULBURY",    T_WATER,    3, { {118,22},{TP_NATDEX(846),23},{TP_NATDEX(834),24} } },
  { "순무",   "MOTOSTOKE",  T_FIRE,     3, { {38,25},{59,25},{TP_NATDEX(851),27} } },
  { "채두",    "STOWSIDE",   T_FIGHTING, 4, { {237,34},{675,34},{TP_NATDEX(865),35},{68,36} } },
  { "포플러",   "BALLONLEA",  T_FAIRY,    4, { {110,36},{303,36},{468,37},{TP_NATDEX(869),38} } },
  { "마쿠와", "CIRCHESTER", T_ROCK,     4, { {689,40},{213,40},{TP_NATDEX(874),41},{TP_NATDEX(839),42} } },
  { "두송",  "SPIKEMUTH",  T_DARK,     4, { {560,44},{687,45},{435,45},{TP_NATDEX(862),46} } },
  { "금랑", "HAMMERLOCKE",T_DRAGON,   4, { {526,46},{330,47},{TP_NATDEX(844),46},{TP_NATDEX(884),48} } },
  { "마리", "CHAMP CUP",  T_DARK,     5, { {510,47},{454,47},{560,47},{TP_NATDEX(877),48},{TP_NATDEX(861),49} } },
  { "비트",   "CHAMP CUP",  T_FAIRY,    4, { {303,51},{282,51},{78,52},{TP_NATDEX(858),53} } },
  { "호브",    "CHAMP CUP",  T_FLYING,   5, { {TP_NATDEX(832),48},{143,47},{TP_NATDEX(871),47},{TP_NATDEX(823),48},{TP_NATDEX(815),49} } },
  { "로즈",   "ROSE TOWER", T_STEEL,    5, { {589,55},{598,55},{601,56},{TP_NATDEX(863),55},{TP_NATDEX(879),57} } },
  { "단델",   "CHAMPION",   T_DRAGON,   6, { {681,62},{TP_NATDEX(887),62},{612,63},{464,64},{TP_NATDEX(812),64},{6,65} } },
};

// PALDEA -- the level-order route used by Scarlet/Violet's open-world gyms.
// Terastallization is deliberately ignored, matching the project's no-gimmicks
// rule. Paldea's real Elite Four + Top Champion fit the common 13-slot ladder.
static const Trainer TRAINERS_PALDEA[TRAINER_COUNT] = {
  { "단풍",    "CORTONDO",    T_BUG,      3, { {TP_NATDEX(919),14},{TP_NATDEX(917),14},{216,15} } },
  { "콜사", "ARTAZON",    T_GRASS,    3, { {548,16},{TP_NATDEX(928),16},{185,17} } },
  { "모야모",    "LEVINCIA",    T_ELECTRIC, 4, { {TP_NATDEX(940),23},{TP_NATDEX(939),23},{404,23},{429,24} } },
  { "곤포",    "CASCARRAFA",  T_WATER,    3, { {TP_NATDEX(976),29},{TP_NATDEX(961),29},{740,30} } },
  { "청목",   "MEDALI",      T_NORMAL,   3, { {775,35},{TP_NATDEX(982),35},{398,36} } },
  { "라임",    "MONTENEVERA", T_GHOST,    4, { {354,41},{778,41},{TP_NATDEX(972),41},{TP_NATDEX(849),42} } },
  { "리파",   "ALFORNADA",   T_PSYCHIC,  4, { {TP_NATDEX(981),44},{282,44},{TP_NATDEX(956),44},{671,45} } },
  { "그루샤",  "GLASEADO",    T_ICE,      4, { {TP_NATDEX(873),47},{614,47},{TP_NATDEX(975),47},{334,48} } },
  { "칠리",    "ELITE 4",     T_GROUND,   5, { {340,57},{323,57},{232,57},{51,57},{TP_NATDEX(980),58} } },
  { "뽀삐",   "ELITE 4",     T_STEEL,    5, { {TP_NATDEX(879),58},{462,58},{437,58},{TP_NATDEX(823),58},{TP_NATDEX(959),59} } },
  { "청목",   "ELITE 4",     T_FLYING,   5, { {357,59},{741,59},{334,59},{398,59},{TP_NATDEX(973),60} } },
  { "팔자크",  "ELITE 4",     T_DRAGON,   5, { {715,60},{612,60},{691,60},{TP_NATDEX(841),60},{TP_NATDEX(998),61} } },
  { "테사",   "CHAMPION",    T_NORMAL,   6, { {TP_NATDEX(956),61},{673,61},{TP_NATDEX(976),61},{713,61},{TP_NATDEX(983),61},{TP_NATDEX(970),62} } },
};

#define GYM_REGIONS 9
static const TrainerSet TRAINER_SETS[GYM_REGIONS] = {
  { TRAINERS_KANTO,  "KANTO",  0 },
  { TRAINERS_JOHTO,  "JOHTO",  1 },
  { TRAINERS_HOENN,  "HOENN",  2 },
  { TRAINERS_SINNOH, "SINNOH", 3 },
  { TRAINERS_UNOVA,  "UNOVA",  4 },
  { TRAINERS_KALOS,  "KALOS",  5 },
  { TRAINERS_ALOLA,  "ALOLA",  6 },
  { TRAINERS_GALAR,  "GALAR",  7 },
  { TRAINERS_PALDEA, "PALDEA", 9 },
};

static inline uint8_t gymRegionDexRegion(uint8_t gymRegion) {
  return TRAINER_SETS[gymRegion % GYM_REGIONS].regionId;
}

// Hard mode reruns the same ladder with perfect IVs and a smarter AI, so the
// teams need no second table -- only the difficulty flag changes.
#define HARD_IV 31
#define EASY_IV 16
