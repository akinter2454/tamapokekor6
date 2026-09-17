#include "digimon.h"
#include "i18n.h"

DigiPet digiPet;

// Digital Monster COLOR Ver.1-5 core rosters. The numeric power is used only
// as a compact balance seed; TamaPoke calculates four independent stats.
#define D(n,v,s,p,y) {n,v,s,p,y}
const DigiSpecies DIGI_SPECIES[] = {
 D("Botamon",1,0,10,0),D("Koromon",1,1,15,0),D("Agumon",1,2,30,0),D("Betamon",1,2,25,3),
 D("Greymon",1,3,50,0),D("Tyranomon",1,3,45,3),D("Devimon",1,3,50,1),D("Meramon",1,3,45,0),D("Airdramon",1,3,50,2),D("Seadramon",1,3,45,3),D("Numemon",1,3,40,3),
 D("MetalGreymon_Virus",1,4,100,0),D("Mamemon",1,4,85,1),D("Monzaemon",1,4,100,3),D("BlitzGreymon",1,5,170,0),D("BanchoMamemon",1,5,150,1),D("ShinMonzaemon",1,5,170,3),
 D("Punimon",2,0,10,0),D("Tsunomon",2,1,15,0),D("Gabumon",2,2,30,1),D("Elecmon",2,2,25,2),
 D("Kabuterimon",2,3,50,1),D("Garurumon",2,3,45,2),D("Angemon",2,3,50,0),D("Yukidarumon",2,3,45,3),D("Birdramon",2,3,50,2),D("Whamon",2,3,45,3),D("Vegimon",2,3,40,3),
 D("SkullGreymon",2,4,100,0),D("MetalMamemon",2,4,85,1),D("Vademon",2,4,100,2),D("SkullMammon",2,5,170,1),D("CresGarurumon",2,5,150,2),D("Ebemon",2,5,170,0),
 D("Poyomon",3,0,10,0),D("Tokomon",3,1,15,0),D("Patamon",3,2,30,2),D("Kunemon",3,2,25,1),
 D("Unimon",3,3,50,2),D("Centalmon",3,3,45,2),D("Ogremon",3,3,50,0),D("Bakemon",3,3,45,2),D("Shellmon",3,3,50,1),D("Drimogemon",3,3,45,0),D("Scumon",3,3,40,3),
 D("Andromon",3,4,100,1),D("Giromon",3,4,85,2),D("Etemon",3,4,100,0),D("Chimairamon",3,4,100,3),D("HiAndromon",3,5,150,1),D("Gokumon",3,5,170,0),D("BanchoLeomon",3,5,180,2),
 D("Yuramon",4,0,10,0),D("Tanemon",4,1,15,0),D("Piyomon",4,2,30,2),D("Palmon",4,2,25,3),
 D("Monochromon",4,3,50,1),D("Cockatrimon",4,3,45,2),D("Leomon",4,3,50,0),D("Kuwagamon",4,3,45,0),D("Coelamon",4,3,50,3),D("Mojyamon",4,3,45,1),D("Nanimon",4,3,40,3),
 D("Megadramon",4,4,100,0),D("Piccolomon",4,4,85,2),D("Digitamamon",4,4,100,3),D("Darkdramon",4,5,170,0),D("BloomLordmon",4,5,150,2),D("Gankoomon",4,5,170,1),
 D("Zurumon",5,0,10,0),D("Pagumon",5,1,15,0),D("Gazimon",5,2,30,0),D("Gizamon",5,2,25,2),
 D("DarkTyranomon",5,3,50,0),D("Cyclomon",5,3,45,1),D("Devidramon",5,3,50,2),D("Tuskmon",5,3,45,0),D("Flymon",5,3,50,2),D("Deltamon",5,3,45,1),D("Raremon",5,3,40,3),
 D("MetalTyranomon",5,4,100,0),D("Nanomon",5,4,85,2),D("ExTyranomon",5,4,100,3),D("Mugendramon",5,5,170,0),D("Raidenmon",5,5,150,2),D("Gaioumon",5,5,180,1),
 // DF/DX entries use version 0 so they have one stable Dex/SD identity even
 // when their materials originate in two different COLOR versions.
 D("Omnimon Alter-S",0,5,220,0),D("Chaosmon",0,5,215,0),D("Millenniummon",0,5,225,3),D("Chaosdramon",0,5,210,1),
// Pendulum COLOR ZERO/1-5. Version codes: P0=10, P1=11 ... P5=15.
// The final entries in each group are combination-only Ultimate+ species.
 D("Bubbmon",11,0,10,3),
 D("Mochimon",11,1,15,0),
 D("Angoramon",11,2,30,1),
 D("Tentomon",11,2,30,2),
 D("Gottsumon",11,2,30,3),
 D("Otamamon",11,2,30,0),
 D("SymbareAngoramon",11,3,50,2),
 D("Kabuterimon",11,3,50,3),
 D("Tortamon",11,3,50,0),
 D("Tailmon",11,3,50,1),
 D("Monochromon",11,3,50,2),
 D("Starmon",11,3,50,3),
 D("Gekomon",11,3,50,0),
 D("Kuwagamon",11,3,50,1),
 D("Lamortmon",11,4,95,3),
 D("AtlurKabuterimonBlue",11,4,95,0),
 D("Jyagamon",11,4,95,1),
 D("Angewomon",11,4,95,2),
 D("Triceramon",11,4,95,3),
 D("Piccolomon",11,4,95,0),
 D("TonosamaGekomon",11,4,95,1),
 D("Okuwamon",11,4,95,2),
 D("Diarbbitmon",11,5,170,0),
 D("HerakleKabuterimon",11,5,170,1),
 D("Blastmon",11,5,170,2),
 D("Holydramon",11,5,170,3),
 D("SaberLeomon",11,5,170,0),
 D("ElDoradimon",11,5,170,1),
 D("MetalEtemon",11,5,170,2),
 D("GranKuwagamon",11,5,170,3),
 D("Mastemon",11,5,220,1),
 D("Tlalocmon",11,5,220,2),
 D("Pitchmon",12,0,10,0),
 D("Pukamon",12,1,15,1),
 D("Gomamon",12,2,30,2),
 D("Jellymon",12,2,30,3),
 D("Ganimon",12,2,30,0),
 D("Shakomon",12,2,30,1),
 D("Rukamon",12,3,50,3),
 D("Ikkakumon",12,3,50,0),
 D("TeslaJellymon",12,3,50,1),
 D("Seadramon",12,3,50,2),
 D("Coelamon",12,3,50,3),
 D("Ebidramon",12,3,50,0),
 D("Octmon",12,3,50,1),
 D("Gesomon",12,3,50,2),
 D("WhamonPerfect",12,4,95,0),
 D("Zudomon",12,4,95,1),
 D("Thetismon",12,4,95,2),
 D("MegaSeadramon",12,4,95,3),
 D("Anomalocarimon",12,4,95,0),
 D("Hangyomon",12,4,95,1),
 D("Dagomon",12,4,95,2),
 D("MarinDevimon",12,4,95,3),
 D("MarinAngemon",12,5,170,1),
 D("Amphimon",12,5,170,2),
 D("Plesiomon",12,5,170,3),
 D("JumboGamemon",12,5,170,0),
 D("MetalSeadramon",12,5,170,1),
 D("Cthyllamon",12,5,170,2),
 D("Pukumon",12,5,170,3),
 D("Vikemon",12,5,170,0),
 D("Aegisdramon",12,5,220,2),
 D("Mitamamon",12,5,220,3),
 D("Mokumon",13,0,10,1),
 D("PetitMeramon",13,1,15,2),
 D("Bakumon",13,2,30,3),
 D("Candmon",13,2,30,0),
 D("Loogamon",13,2,30,1),
 D("PicoDevimon",13,2,30,2),
 D("Hanumon",13,3,50,0),
 D("Garurumon",13,3,50,1),
 D("Meramon",13,3,50,2),
 D("Wizarmon",13,3,50,3),
 D("Devimon",13,3,50,0),
 D("Bakemon",13,3,50,1),
 D("Dokugumon",13,3,50,2),
 D("Loogarmon",13,3,50,3),
 D("Mammon",13,4,95,1),
 D("WereGarurumon",13,4,95,2),
 D("DeathMeramon",13,4,95,3),
 D("Pumpmon",13,4,95,0),
 D("Soloogarmon",13,4,95,1),
 D("Vamdemon",13,4,95,2),
 D("Fantomon",13,4,95,3),
 D("LadyDevimon",13,4,95,0),
 D("SkullMammon",13,5,170,2),
 D("Anubimon",13,5,170,3),
 D("Boltmon",13,5,170,0),
 D("NoblePumpmon",13,5,170,1),
 D("Fenriloogamon",13,5,170,2),
 D("Callismon",13,5,170,3),
 D("Piemon",13,5,170,0),
 D("Demon",13,5,170,1),
 D("Mastemon",13,5,220,3),
 D("Voltobautamon",13,5,220,0),
 D("Nyokimon",14,0,10,2),
 D("Pyocomon",14,1,15,3),
 D("Piyomon",14,2,30,0),
 D("Pteromon",14,2,30,1),
 D("Palmon",14,2,30,2),
 D("Floramon",14,2,30,3),
 D("Mushmon",14,2,30,0),
 D("Vdramon",14,3,50,1),
 D("Birdramon",14,3,50,2),
 D("Galemon",14,3,50,3),
 D("Togemon",14,3,50,0),
 D("Kiwimon",14,3,50,1),
 D("RedVegimon",14,3,50,2),
 D("Woodmon",14,3,50,3),
 D("AeroVdramon",14,4,95,2),
 D("Garudamon",14,4,95,3),
 D("GrandGalemon",14,4,95,0),
 D("Lilimon",14,4,95,1),
 D("Blossomon",14,4,95,2),
 D("Delumon",14,4,95,3),
 D("Jyureimon",14,4,95,0),
 D("Gerbemon",14,4,95,1),
 D("UlforceVdramon",14,5,170,3),
 D("Hououmon",14,5,170,0),
 D("Zephagamon",14,5,170,1),
 D("Griffomon",14,5,170,2),
 D("Rosemon",14,5,170,3),
 D("Rafflesimon",14,5,170,0),
 D("Hydramon",14,5,170,1),
 D("Pinochimon",14,5,170,2),
 D("Mitamamon",14,5,220,0),
 D("Cernumon",14,5,220,1),
 D("Choromon",15,0,10,3),
 D("Caprimon",15,1,15,0),
 D("ToyAgumon",15,2,30,1),
 D("Kokuwamon",15,2,30,2),
 D("Hagurumon",15,2,30,3),
 D("Commandramon",15,2,30,0),
 D("Greymon",15,3,50,2),
 D("Revolmon",15,3,50,3),
 D("Clockmon",15,3,50,0),
 D("Thunderballmon",15,3,50,1),
 D("Tankmon",15,3,50,2),
 D("Guardromon",15,3,50,3),
 D("Mechanorimon",15,3,50,0),
 D("HiCommandramon",15,3,50,1),
 D("MetalGreymon",15,4,95,3),
 D("Andromon",15,4,95,0),
 D("Cyberdramon",15,4,95,1),
 D("BigMamemon",15,4,95,2),
 D("Knightmon",15,4,95,3),
 D("Megadramon",15,4,95,0),
 D("WaruMonzaemon",15,4,95,1),
 D("Cargodramon",15,4,95,2),
 D("WarGreymon",15,5,170,0),
 D("HiAndromon",15,5,170,1),
 D("MetalGarurumon",15,5,170,2),
 D("Brigadramon",15,5,170,3),
 D("ZekeGreymon",15,5,170,0),
 D("Mugendramon",15,5,170,1),
 D("VenomVamdemon",15,5,170,2),
 D("Ragnamon",15,5,170,3),
 D("Omegamon",15,5,220,1),
 D("Chaosdramon",15,5,220,2),
 D("YukimiBotamon",10,0,10,2),
 D("Nyaromon",10,1,15,3),
 D("Agumon",10,2,30,0),
 D("Plotmon",10,2,30,1),
 D("Gabumon",10,2,30,2),
 D("Gammamon",10,2,30,3),
 D("Greymon",10,3,50,1),
 D("Leomon",10,3,50,2),
 D("Tailmon",10,3,50,3),
 D("Garurumon",10,3,50,0),
 D("Angemon",10,3,50,1),
 D("BetelGammamon",10,3,50,2),
 D("Igamon",10,3,50,3),
 D("KausGammamon",10,3,50,0),
 D("WezenGammamon",10,3,50,1),
 D("GulusGammamon",10,3,50,2),
 D("MetalGreymon",10,4,95,2),
 D("Asuramon",10,4,95,3),
 D("WereGarurumon",10,4,95,0),
 D("HolyAngemon",10,4,95,1),
 D("Angewomon",10,4,95,2),
 D("Canoweissmon",10,4,95,3),
 D("MetalMamemon",10,4,95,0),
 D("Regulusmon",10,4,95,1),
 D("WarGreymon",10,5,170,3),
 D("Siriusmon",10,5,170,0),
 D("Dominimon",10,5,170,1),
 D("MetalGarurumon",10,5,170,2),
 D("Quantumon",10,5,170,3),
 D("Arcturusmon",10,5,170,0),
 D("Omegamon",10,5,220,0),
 D("Mastemon",10,5,220,1),
 D("Proximamon",10,5,220,2),
};
#undef D
const uint16_t DIGI_SPECIES_COUNT = sizeof(DIGI_SPECIES)/sizeof(DIGI_SPECIES[0]);
static_assert(DIGI_SPECIES_COUNT <= DIGI_SPECIES_CAP, "increase DIGI_SPECIES_CAP");

// Display-only Korean names. DIGI_SPECIES[].name deliberately remains the
// stable ASCII key used by type rules, serial tooling and SD sprite matching.
static const char *const DIGI_NAMES_KO[] = {
 "보타몬","코로몬","아구몬","베타몬","그레이몬","티라노몬","데블몬","메라몬","에어드라몬","시드라몬","누메몬",
 "메탈그레이몬(바이러스)","마메몬","몬자에몬","블리츠그레이몬","반쵸마메몬","신몬자에몬",
 "푸니몬","츠노몬","가부몬","에렉몬","캅테리몬","가루몬","엔젤몬","유키다루몬","버드라몬","웨이몬","베지몬",
 "스컬그레이몬","메탈마메몬","베이더몬","스컬맘몬","크레스가루몬","이바몬",
 "포요몬","토코몬","파닥몬","쿠네몬","유니몬","켄타루몬","우가몬","바케몬","쉘몬","드리모게몬","스카몬",
 "안드로몬","기로몬","에테몬","키메라몬","하이안드로몬","고쿠몬","반쵸레오몬",
 "유라몬","타네몬","피요몬","팔몬","모노크로몬","코카토리몬","레오몬","쿠가몬","코엘라몬","모쟈몬","나니몬",
 "메가드라몬","픽콜몬","디지타마몬","다크드라몬","블룸로드몬","간쿠몬",
 "즈루몬","파구몬","가지몬","기자몬","다크티라노몬","사이크로몬","데비드라몬","터스크몬","플라이몬","델타몬","레어몬",
 "메탈티라노몬","나노몬","엑스티라노몬","무겐드라몬","라이덴몬","가이오몬",
 "오메가몬 Alter-S","카오스몬","밀레니엄몬","카오스드라몬",
 "버브몬",
 "모치몬",
 "앙고라몬",
 "텐타몬",
 "곳츠몬",
 "올챙몬",
 "심바레앙고라몬",
 "캅테리몬",
 "토터몬",
 "가트몬",
 "모노크로몬",
 "스타몬",
 "개굴몬",
 "쿠가몬",
 "라모르몬",
 "아트라캅테리몬(청)",
 "쟈가몬",
 "엔젤우몬",
 "트리케라몬",
 "픽콜몬",
 "왕개굴몬",
 "왕쿠가몬",
 "디아르비트몬",
 "헤라클레스캅테리몬",
 "블래스트몬",
 "홀리드라몬",
 "샤벨레오몬",
 "엘도라디몬",
 "메탈에테몬",
 "그랑쿠가몬",
 "마스테몬",
 "틀랄록몬",
 "피치몬",
 "푸카몬",
 "쉬라몬",
 "젤리몬",
 "가니몬",
 "샤코몬",
 "루카몬",
 "원뿔몬",
 "테슬라젤리몬",
 "시드라몬",
 "코엘라몬",
 "에비드라몬",
 "옥토몬",
 "게소몬",
 "웨이몬(완전체)",
 "쥬드몬",
 "테티스몬",
 "메가시드라몬",
 "아노말로카리몬",
 "항가몬",
 "다고몬",
 "마린데블몬",
 "마린엔젤몬",
 "암피몬",
 "플레시오몬",
 "점보가메몬",
 "메탈시드라몬",
 "크틸라몬",
 "푸쿠몬",
 "바이킹몬",
 "이지스드라몬",
 "미타마몬",
 "모쿠몬",
 "푸치메라몬",
 "바쿠몬",
 "캔들몬",
 "루가몬",
 "피코데블몬",
 "하누몬",
 "가루몬",
 "메라몬",
 "위자몬",
 "데블몬",
 "바케몬",
 "도쿠구몬",
 "루가루몬",
 "맘몬",
 "워가루몬",
 "데스메라몬",
 "펌프몬",
 "솔루가루몬",
 "묘티스몬",
 "팬텀몬",
 "레이디데블몬",
 "스컬맘몬",
 "아누비스몬",
 "볼트몬",
 "노블펌프몬",
 "펜리루가몬",
 "콜리스몬",
 "피에몬",
 "마왕몬",
 "마스테몬",
 "볼토바우타몬",
 "뇨키몬",
 "피요코몬",
 "피요몬",
 "프테로몬",
 "팔몬",
 "플로라몬",
 "머쉬몬",
 "브이드라몬",
 "버드라몬",
 "게일몬",
 "니드몬",
 "키위몬",
 "레드베지몬",
 "우드몬",
 "에어로브이드라몬",
 "가루다몬",
 "그랜드게일몬",
 "릴리몬",
 "블로섬몬",
 "데라몬",
 "쥬레이몬",
 "가비지몬",
 "알포스브이드라몬",
 "호우오우몬",
 "제파가몬",
 "그리포몬",
 "로제몬",
 "라플레시몬",
 "히드라몬",
 "피노키몬",
 "미타마몬",
 "케르누몬",
 "쵸로몬",
 "카프리몬",
 "토이아구몬",
 "코쿠와몬",
 "톱니몬",
 "코만드라몬",
 "그레이몬",
 "리볼몬",
 "클락몬",
 "썬더볼몬",
 "탱크몬",
 "가드로몬",
 "메카노몬",
 "하이코만드라몬",
 "메탈그레이몬",
 "안드로몬",
 "사이버드라몬",
 "빅마메몬",
 "나이트몬",
 "메가드라몬",
 "와루몬자에몬",
 "카고드라몬",
 "워그레이몬",
 "하이안드로몬",
 "메탈가루몬",
 "브리가드라몬",
 "지크그레이몬",
 "무겐드라몬",
 "베놈묘티스몬",
 "라그나몬",
 "오메가몬",
 "카오스드라몬",
 "유키미보타몬",
 "냐로몬",
 "아구몬",
 "플롯트몬",
 "가부몬",
 "감마몬",
 "그레이몬",
 "레오몬",
 "가트몬",
 "가루몬",
 "엔젤몬",
 "베텔감마몬",
 "이가몬",
 "카우스감마몬",
 "웨즌감마몬",
 "굴루스감마몬",
 "메탈그레이몬",
 "아수라몬",
 "워가루몬",
 "홀리엔젤몬",
 "엔젤우몬",
 "카노바이스몬",
 "메탈마메몬",
 "레굴루스몬",
 "워그레이몬",
 "시리우스몬",
 "도미니몬",
 "메탈가루몬",
 "퀀텀몬",
 "아크투루스몬",
 "오메가몬",
 "마스테몬",
 "프로시마몬",
};
static_assert(sizeof(DIGI_NAMES_KO)/sizeof(DIGI_NAMES_KO[0]) == sizeof(DIGI_SPECIES)/sizeof(DIGI_SPECIES[0]),
              "Korean Digimon name table must match species catalog");
const char *digimonNameKo(uint16_t index){return index<DIGI_SPECIES_COUNT?DIGI_NAMES_KO[index]:"?";}

static uint16_t firstOf(uint8_t ver, uint8_t stage) {
  for (uint16_t i=0;i<DIGI_SPECIES_COUNT;i++) if(DIGI_SPECIES[i].version==ver&&DIGI_SPECIES[i].stage==stage) return i;
  return 0;
}
static uint8_t countOf(uint8_t ver, uint8_t stage) {
  uint8_t n=0; for(uint16_t i=0;i<DIGI_SPECIES_COUNT;i++) n += DIGI_SPECIES[i].version==ver&&DIGI_SPECIES[i].stage==stage; return n;
}

void DigiPet::begin(){ prefs.begin("digipet",false); enabled=prefs.getBool("on",false);egg=prefs.getBool("egg",false);eggTaps=prefs.getUChar("etap",0);version=prefs.getUChar("ver",1);if(!((version>=1&&version<=5)||(version>=10&&version<=15)))version=1;speciesId=prefs.getUShort("id",firstOf(version,0));if(speciesId>=DIGI_SPECIES_COUNT)speciesId=firstOf(version,0);prefs.getBytes("iv",iv,sizeof(iv));prefs.getBytes("tr",training,sizeof(training));size_t rl=prefs.getBytesLength("reg");if(rl) prefs.getBytes("reg",registered,min(rl,sizeof(registered)));size_t bl=prefs.getBytesLength("best");if(bl)prefs.getBytes("best",bestLevel,min(bl,sizeof(bestLevel)));if(prefs.getBytesLength("moves")==sizeof(moves))prefs.getBytes("moves",moves,sizeof(moves));else relearnMoves();levelMinutes=prefs.getUInt("mins",0);lastTick=millis();recordCurrentLevel();}
void DigiPet::recordCurrentLevel(){if(enabled&&!egg&&speciesId<DIGI_SPECIES_COUNT){uint8_t lv=level();if(lv>bestLevel[speciesId])bestLevel[speciesId]=lv;}}
void DigiPet::save(){recordCurrentLevel();prefs.putBool("on",enabled);prefs.putBool("egg",egg);prefs.putUChar("etap",eggTaps);prefs.putUChar("ver",version);prefs.putUShort("id",speciesId);prefs.putBytes("iv",iv,sizeof(iv));prefs.putBytes("tr",training,sizeof(training));prefs.putBytes("reg",registered,sizeof(registered));prefs.putBytes("best",bestLevel,sizeof(bestLevel));prefs.putBytes("moves",moves,sizeof(moves));prefs.putUInt("mins",levelMinutes);}
void DigiPet::start(uint8_t v){ if(!((v>=1&&v<=5)||(v>=10&&v<=15)))v=1;version=v;speciesId=firstOf(version,0);levelMinutes=0;egg=false;eggTaps=0;for(int i=0;i<4;i++){iv[i]=random(32);training[i]=0;}enabled=true;registered[speciesId>>3]|=1<<(speciesId&7);relearnMoves();save(); }
void DigiPet::startEgg(uint8_t v){if(!((v>=1&&v<=5)||(v>=10&&v<=15)))v=1;version=v;speciesId=firstOf(version,0);enabled=true;egg=true;eggTaps=0;levelMinutes=0;for(int i=0;i<4;i++)training[i]=0;save();}
bool DigiPet::tapEgg(){if(!enabled||!egg)return false;if(++eggTaps<3){save();return false;}egg=false;eggTaps=0;levelMinutes=0;for(int i=0;i<4;i++){iv[i]=random(32);training[i]=0;}relearnMoves();registered[speciesId>>3]|=1<<(speciesId&7);save();return true;}
void DigiPet::update(uint32_t now){if(!lastTick)lastTick=now;if(egg)return;uint32_t dm=now-lastTick;if(dm>=60000){levelMinutes+=dm/60000;lastTick+=dm/60000*60000;save();}}
uint8_t DigiPet::level()const{uint32_t n=1+levelMinutes/30;return n>100?100:n;}
const DigiSpecies&DigiPet::species()const{return DIGI_SPECIES[speciesId<DIGI_SPECIES_COUNT?speciesId:0];}
void DigiPet::train(DigiTrain k,uint8_t amount){if(k>3)return;uint16_t n=training[k]+amount;training[k]=n>999?999:n;save();}
uint16_t DigiPet::stat(DigiTrain k)const{const DigiSpecies&s=species();uint16_t base=12+s.stage*12+s.power/4;uint16_t bias=(s.style==k)?(8+s.stage*4):0;return base+bias+iv[k]*level()/100+training[k]/2+level();}
static bool hasWord(const char*n,const char*w){return strstr(n,w)!=nullptr;}
static uint8_t digiThemeType(const DigiSpecies&s){
 // Device families supply a sensible fallback for names without an obvious
 // elemental cue, while style keeps siblings from collapsing to one type.
 static const uint8_t dmc[5][4]={
  {T_FIRE,T_DRAGON,T_GROUND,T_NORMAL},{T_ICE,T_ELECTRIC,T_FLYING,T_NORMAL},
  {T_FIGHTING,T_PSYCHIC,T_ROCK,T_NORMAL},{T_GRASS,T_FLYING,T_BUG,T_FAIRY},
  {T_DARK,T_POISON,T_STEEL,T_GHOST}};
 static const uint8_t pen[6][4]={
  {T_FIGHTING,T_STEEL,T_FAIRY,T_DRAGON},{T_GRASS,T_BUG,T_GROUND,T_FAIRY},
  {T_WATER,T_ICE,T_POISON,T_DRAGON},{T_DARK,T_GHOST,T_POISON,T_PSYCHIC},
  {T_FLYING,T_GRASS,T_DRAGON,T_BUG},{T_STEEL,T_ELECTRIC,T_ROCK,T_GROUND}};
 if(s.version>=1&&s.version<=5)return dmc[s.version-1][s.style&3];
 if(s.version>=10&&s.version<=15)return pen[s.version-10][s.style&3];
 return T_NORMAL;
}
uint8_t DigiPet::type1()const{
 const char*n=species().name;
 if(speciesId==DIGI_OMNIMON_ALTER_S||speciesId==DIGI_CHAOSDRAMON)return T_STEEL;
 if(speciesId==DIGI_CHAOSMON)return T_FIGHTING;
 if(speciesId==DIGI_MILLENNIUMMON)return T_DARK;
 if(hasWord(n,"Sea")||hasWord(n,"Whamon")||hasWord(n,"Coela")||hasWord(n,"Shell")||hasWord(n,"Gizamon")||hasWord(n,"Marin")||hasWord(n,"Plesio")||hasWord(n,"Zudo")||hasWord(n,"Gomamon")||hasWord(n,"Oct")||hasWord(n,"Geso")||hasWord(n,"Gani")||hasWord(n,"Ruka")||hasWord(n,"Puku")||hasWord(n,"Amphi")||hasWord(n,"Divemon")||hasWord(n,"Syako")||hasWord(n,"Aegi"))return T_WATER;
 if(hasWord(n,"Kabuteri")||hasWord(n,"Kuwaga")||hasWord(n,"Kunemon")||hasWord(n,"Flymon")||hasWord(n,"Tentomon")||hasWord(n,"Dokugu")||hasWord(n,"Archn")||hasWord(n,"Parasimon"))return T_BUG;
 if(hasWord(n,"Palmon")||hasWord(n,"Tanemon")||hasWord(n,"Yuramon")||hasWord(n,"Vegi")||hasWord(n,"Bloom")||hasWord(n,"Flora")||hasWord(n,"Toge")||hasWord(n,"Wood")||hasWord(n,"Mush")||hasWord(n,"Lili")||hasWord(n,"Blosso")||hasWord(n,"Jyurei")||hasWord(n,"Hydra")||hasWord(n,"Cernu"))return T_GRASS;
 if(hasWord(n,"Elec")||hasWord(n,"Raiden")||hasWord(n,"Giromon")||hasWord(n,"Thunder")||hasWord(n,"Pulse")||hasWord(n,"Tesla")||hasWord(n,"Volto"))return T_ELECTRIC;
 if(hasWord(n,"Numemon")||hasWord(n,"Scum")||hasWord(n,"Rare")||hasWord(n,"Dokugu")||hasWord(n,"Archn")||hasWord(n,"Parasimon"))return T_POISON;
 if(hasWord(n,"Gottsu")||hasWord(n,"Gole")||hasWord(n,"Drimoge")||hasWord(n,"ElDoradi")||hasWord(n,"Tricera")||hasWord(n,"Monochro")||hasWord(n,"Torta"))return T_GROUND;
 if(hasWord(n,"Blastmon")||hasWord(n,"Crag")||hasWord(n,"Gokumon"))return T_ROCK;
 if(hasWord(n,"Bake")||hasWord(n,"Fanto")||hasWord(n,"Soul")||hasWord(n,"Ghost")||hasWord(n,"Pump")||hasWord(n,"Candmon"))return T_GHOST;
 if(hasWord(n,"Dev")||hasWord(n,"Skull")||hasWord(n,"Dark")||hasWord(n,"Vamde")||hasWord(n,"Piemon")||hasWord(n,"Demon")||hasWord(n,"Looga")||hasWord(n,"Fenri")||hasWord(n,"Callis"))return T_DARK;
 if(hasWord(n,"Ange")||hasWord(n,"Monza")||hasWord(n,"Piccolo")||hasWord(n,"Tailmon")||hasWord(n,"Plotmon")||hasWord(n,"Mitama"))return T_FAIRY;
 if(hasWord(n,"Mame")||hasWord(n,"Andro")||hasWord(n,"Nano")||hasWord(n,"Mugen")||hasWord(n,"Metal")||hasWord(n,"Mechano")||hasWord(n,"Guardro")||hasWord(n,"Ragna"))return T_STEEL;
 if(hasWord(n,"Agu")||hasWord(n,"Grey")||hasWord(n,"Tyrano")||hasWord(n,"Mera")||hasWord(n,"Bird")||hasWord(n,"Blitz"))return T_FIRE;
 if(hasWord(n,"Garuru")||hasWord(n,"Yukidaru")||hasWord(n,"Cres"))return T_ICE;
 if(hasWord(n,"Airdra")||hasWord(n,"Megadra")||hasWord(n,"Darkdra")||hasWord(n,"Gaiou"))return T_DRAGON;
 if(hasWord(n,"Leomon")||hasWord(n,"Ogremon")||hasWord(n,"Cental")||hasWord(n,"Drimoge"))return T_FIGHTING;
 if(hasWord(n,"Patamon")||hasWord(n,"Unimon")||hasWord(n,"Piyomon")||hasWord(n,"Cockatri"))return T_FLYING;
 if(hasWord(n,"Vademon")||hasWord(n,"Ebemon")||hasWord(n,"Digitama")||hasWord(n,"Bakumon")||hasWord(n,"Wisemon")||hasWord(n,"Wizar")||hasWord(n,"Quantum"))return T_PSYCHIC;
 return digiThemeType(species());
}
uint8_t DigiPet::type2()const{
 const char*n=species().name,t=type1();
 if(speciesId==DIGI_OMNIMON_ALTER_S)return T_FIRE;
 if(speciesId==DIGI_CHAOSMON)return T_DARK;
 if(speciesId==DIGI_MILLENNIUMMON)return T_PSYCHIC;
 if(speciesId==DIGI_CHAOSDRAMON)return T_DRAGON;
 if((hasWord(n,"Grey")||hasWord(n,"Tyrano"))&&t!=T_DRAGON)return T_DRAGON;
 if((hasWord(n,"Metal")||hasWord(n,"Andro")||hasWord(n,"Mugen")||hasWord(n,"Blitz"))&&t!=T_STEEL)return T_STEEL;
 if((hasWord(n,"Dev")||hasWord(n,"Skull")||hasWord(n,"Dark"))&&t!=T_DARK)return T_DARK;
 if((hasWord(n,"Bird")||hasWord(n,"Airdra"))&&t!=T_FLYING)return T_FLYING;
 if((hasWord(n,"Flymon")||hasWord(n,"Kuwaga")||hasWord(n,"Dokugu")||hasWord(n,"Archn"))&&t!=T_POISON)return T_POISON;
 if((hasWord(n,"Sea")||hasWord(n,"Marin")||hasWord(n,"Whamon")||hasWord(n,"Plesio"))&&t!=T_ICE)return T_ICE;
 if((hasWord(n,"Palmon")||hasWord(n,"Flora")||hasWord(n,"Vegi")||hasWord(n,"Toge"))&&t!=T_POISON)return T_POISON;
 if((hasWord(n,"Gottsu")||hasWord(n,"Gole")||hasWord(n,"Blastmon"))&&t!=T_ROCK)return T_ROCK;
 if((hasWord(n,"Ange")||hasWord(n,"Tailmon")||hasWord(n,"Houou"))&&t!=T_FLYING)return T_FLYING;
 if((hasWord(n,"Bake")||hasWord(n,"Fanto")||hasWord(n,"Pump"))&&t!=T_GHOST)return T_GHOST;
 return T_NONE;
}
static void typeMoves(uint8_t t,uint8_t*out){
 switch(t){
  case T_FIRE: out[0]=MV_EMBER;out[1]=MV_FIRE_PUNCH;out[2]=MV_FLAMETHROWER;out[3]=MV_FIRE_BLAST;break;
  case T_WATER: out[0]=MV_BUBBLE;out[1]=MV_WATER_GUN;out[2]=MV_SURF;out[3]=MV_HYDRO_PUMP;break;
  case T_ELECTRIC: out[0]=MV_SPARK;out[1]=MV_THUNDERSHOCK;out[2]=MV_THUNDERBOLT;out[3]=MV_THUNDER;break;
  case T_GRASS: out[0]=MV_ABSORB;out[1]=MV_VINE_WHIP;out[2]=MV_MEGA_DRAIN;out[3]=MV_SOLAR_BEAM;break;
  case T_ICE: out[0]=MV_AURORA_BEAM;out[1]=MV_ICE_PUNCH;out[2]=MV_ICE_BEAM;out[3]=MV_BLIZZARD;break;
  case T_FIGHTING: out[0]=MV_KARATE_CHOP;out[1]=MV_ROCK_SMASH;out[2]=MV_BULK_UP;out[3]=MV_HI_JUMP_KICK;break;
  case T_BUG: out[0]=MV_BUG_BITE;out[1]=MV_PIN_MISSILE;out[2]=MV_X_SCISSOR;out[3]=MV_MEGAHORN;break;
  case T_FLYING: out[0]=MV_PECK;out[1]=MV_WING_ATTACK;out[2]=MV_DRILL_PECK;out[3]=MV_AEROBLAST;break;
  case T_PSYCHIC: out[0]=MV_CONFUSION;out[1]=MV_PSYBEAM;out[2]=MV_PSYCHIC;out[3]=MV_PSYSTRIKE;break;
  case T_DRAGON: out[0]=MV_DRAGON_RAGE;out[1]=MV_DRAGON_CLAW;out[2]=MV_OUTRAGE;out[3]=MV_DRAGON_ENERGY;break;
  case T_DARK: out[0]=MV_BITE;out[1]=MV_CRUNCH;out[2]=MV_DARK_PULSE;out[3]=MV_KOWTOW_CLEAVE;break;
  case T_STEEL: out[0]=MV_TACKLE;out[1]=MV_IRON_HEAD;out[2]=MV_FLASH_CANNON;out[3]=MV_SUNSTEEL_STRIKE;break;
  case T_FAIRY: out[0]=MV_POUND;out[1]=MV_DAZZLE_GLEAM;out[2]=MV_PLAY_ROUGH;out[3]=MV_MOONBLAST;break;
  default: out[0]=MV_TACKLE;out[1]=MV_QUICK_ATTACK;out[2]=MV_BODY_SLAM;out[3]=MV_HYPER_BEAM;break;
 }
}
uint8_t DigiPet::signatureMove()const{return (uint8_t)(MV_DIGI_PULSE+(type1()<TYPE_COUNT?type1():T_NORMAL));}
void DigiPet::relearnMoves(){uint8_t pool[4];typeMoves(type1(),pool);uint8_t unlocked=1+species().stage/2;if(unlocked>3)unlocked=3;for(uint8_t i=0;i<3;i++)moves[i]=i<unlocked?pool[i]:MV_NONE;if(type2()!=T_NONE&&unlocked>1){uint8_t alt[4];typeMoves(type2(),alt);moves[unlocked-1]=alt[unlocked-1];}moves[3]=signatureMove();}
static uint16_t pendulumFusionTarget(uint16_t i,uint8_t lv,const uint8_t*best);
uint16_t DigiPet::fusionTarget()const{
 if(egg)return DIGI_NO_FUSION;const uint8_t lv=level();
 if(lv>=55&&((speciesId==14&&bestLevel[32]>=55)||(speciesId==32&&bestLevel[14]>=55)))return DIGI_OMNIMON_ALTER_S;
 if(lv>=55&&((speciesId==51&&bestLevel[66]>=55)||(speciesId==66&&bestLevel[51]>=55)))return DIGI_CHAOSMON;
 if(lv>=55&&((speciesId==48&&bestLevel[83]>=55)||(speciesId==83&&bestLevel[48]>=55)))return DIGI_MILLENNIUMMON;
 if(speciesId==83&&lv>=60&&training[DIGI_ATK]>=80&&training[DIGI_DEF]>=60)return DIGI_CHAOSDRAMON;
 return pendulumFusionTarget(speciesId,lv,bestLevel);
}
bool DigiPet::canEvolve()const{if(egg)return false;if(fusionTarget()!=DIGI_NO_FUSION)return true;uint8_t s=species().stage;if(s>=DIGI_ULTIMATE)return false;static const uint8_t need[]={2,5,12,25,45};static const uint16_t tr[]={0,0,4,8,12};uint32_t sum=training[0]+training[1]+training[2]+training[3];return level()>=need[s]&&sum>=tr[s];}
uint16_t DigiPet::chooseEvolution()const{return digimonEvolutionTarget(speciesId,level(),training[0],training[1],training[2],training[3],bestLevel);}
bool DigiPet::evolve(){if(!canEvolve())return false;recordCurrentLevel();uint16_t next=chooseEvolution();if(next==speciesId)return false;speciesId=next;for(int i=0;i<4;i++)training[i]=0;relearnMoves();registered[speciesId>>3]|=1<<(speciesId&7);save();return true;}
uint16_t DigiPet::registeredCount()const{uint16_t n=0;for(uint16_t i=0;i<DIGI_SPECIES_COUNT;i++)if(isRegistered(i))n++;return n;}
uint16_t DigiPet::displayIndex()const{uint16_t n=0;for(uint16_t i=0;i<speciesId;i++)if(DIGI_SPECIES[i].version==version)n++;return n;}

const char *creatureName(int16_t id) {
  if (isDigimonId(id)) return digimonNameKo(digimonIndex(id));
  return (id >= 1 && id <= DEX_COUNT) ? localizedSpeciesName(id) : "?";
}

static uint8_t digiPrimary(uint16_t i) {
  DigiPet p; p.speciesId=i; return p.type1();
}
uint8_t creatureType1(int16_t id) { return isDigimonId(id) ? digiPrimary(digimonIndex(id)) : DEX_TBL[id].type1; }
uint8_t creatureType2(int16_t id) {
  if (!isDigimonId(id)) return DEX_TBL[id].type2;
  DigiPet p; p.speciesId=digimonIndex(id); return p.type2();
}
static uint8_t digiBase(uint16_t i,uint8_t stat) {
  const DigiSpecies&s=DIGI_SPECIES[i];
  uint16_t v=26+s.stage*18+s.power/7;
  if(s.style==stat)v+=14+s.stage*3;
  if(stat==DIGI_HP)v+=8;
  return v>190?190:(uint8_t)v;
}
uint8_t creatureBaseAtk(int16_t id){return isDigimonId(id)?digiBase(digimonIndex(id),DIGI_ATK):DEX_TBL[id].bAtk;}
uint8_t creatureBaseDef(int16_t id){return isDigimonId(id)?digiBase(digimonIndex(id),DIGI_DEF):DEX_TBL[id].bDef;}
uint8_t creatureBaseSpe(int16_t id){return isDigimonId(id)?digiBase(digimonIndex(id),DIGI_SPE):DEX_TBL[id].bSpe;}
uint8_t creatureBaseHp(int16_t id){return isDigimonId(id)?digiBase(digimonIndex(id),DIGI_HP):DEX_TBL[id].bHp;}
uint8_t creatureBaseSpA(int16_t id){return isDigimonId(id)?digiBase(digimonIndex(id),DIGI_ATK):DEX_TBL[id].bSpA;}
uint8_t creatureBaseSpD(int16_t id){return isDigimonId(id)?digiBase(digimonIndex(id),DIGI_DEF):DEX_TBL[id].bSpD;}
uint16_t creatureAccent(int16_t id){return isDigimonId(id)?(uint16_t)(0x3DFF+(digimonIndex(id)%5)*0x1800):DEX_TBL[id].accent;}
uint8_t digimonSignatureMove(uint16_t i){return (uint8_t)(MV_DIGI_PULSE+(digiPrimary(i)<TYPE_COUNT?digiPrimary(i):T_NORMAL));}

static uint8_t digiCoverageType(uint16_t i,uint8_t t1,uint8_t t2){
 // A small role-themed coverage pool expands move variety without handing
 // every Digimon every move. ATK/DEF/SPE/HP styles lean toward different tools.
 static const uint8_t role[4][4]={
  {T_FIGHTING,T_GROUND,T_FIRE,T_DRAGON},{T_ROCK,T_STEEL,T_WATER,T_ICE},
  {T_FLYING,T_ELECTRIC,T_BUG,T_POISON},{T_PSYCHIC,T_FAIRY,T_GHOST,T_GRASS}};
 const DigiSpecies&s=DIGI_SPECIES[i];uint8_t start=(uint8_t)((i+s.version+s.stage)&3);
 for(uint8_t k=0;k<4;k++){uint8_t t=role[s.style&3][(start+k)&3];if(t!=t1&&t!=t2)return t;}
 return T_NORMAL;
}

// Digimon borrow the existing Pokemon move dataset instead of receiving three
// copies of their digital move. The pool is type-compatible and grows stronger
// with level/stage; the default three are shuffled deterministically from the
// species and IVs, so saves remain stable while individuals can differ.
uint8_t digimonLearnableMoves(uint16_t i,uint8_t lv,uint8_t*out,uint8_t max){
 if(i>=DIGI_SPECIES_COUNT||!out||!max)return 0;
 uint8_t w=0,t1=digiPrimary(i);DigiPet p;p.speciesId=i;uint8_t t2=p.type2(),t3=digiCoverageType(i,t1,t2);
 out[w++]=digimonSignatureMove(i);
 uint16_t cap=45u+DIGI_SPECIES[i].stage*18u+lv/3u;if(cap>150)cap=150;
 for(uint16_t mv=1;mv<MV_DIGI_PULSE&&w<max;mv++){
  const MoveEntry&m=MOVE_TBL[mv];
  if(mv==MV_STRUGGLE)continue;
  if(m.type!=t1&&m.type!=t2&&m.type!=t3&&m.type!=T_NORMAL)continue;
  if(m.cat!=MC_STATUS&&m.power&&m.power>cap)continue;
  if(lv<20&&(m.effect==EF_RECHARGE||m.effect==EF_CHARGE))continue;
  out[w++]=(uint8_t)mv;
 }
 return w;
}
static uint32_t digiMoveRand(uint32_t&s){s=s*1664525UL+1013904223UL;return s;}
void digimonDefaultMoves(uint16_t i,uint8_t lv,uint8_t ia,uint8_t id,uint8_t is,uint8_t ih,uint8_t out[4]){
 for(uint8_t k=0;k<4;k++)out[k]=0;if(i>=DIGI_SPECIES_COUNT)return;
 uint8_t pool[MV_DIGI_PULSE];uint8_t n=digimonLearnableMoves(i,lv,pool,sizeof(pool));
 uint32_t seed=0xD161B00BUL^(uint32_t)(i+1)*2654435761UL^((uint32_t)ia<<24)^((uint32_t)id<<16)^((uint32_t)is<<8)^ih;
 // Slot 0 always starts with a damaging primary-type Pokemon move when one is
 // available, then slots 1-2 are deterministic random compatible moves.
 uint8_t primary[MV_DIGI_PULSE],pn=0,t1=digiPrimary(i);
 for(uint8_t k=1;k<n;k++)if(MOVE_TBL[pool[k]].type==t1&&MOVE_TBL[pool[k]].cat!=MC_STATUS)primary[pn++]=pool[k];
 if(pn)out[0]=primary[digiMoveRand(seed)%pn];
 for(uint8_t k=n;k>2;k--){uint8_t j=1+(digiMoveRand(seed)%(k-1));uint8_t q=pool[k-1];pool[k-1]=pool[j];pool[j]=q;}
 uint8_t w=out[0]?1:0;
 for(uint8_t k=1;k<n&&w<3;k++){
  uint8_t mv=pool[k];bool dup=false;for(uint8_t x=0;x<w;x++)if(out[x]==mv)dup=true;
  if(!dup)out[w++]=mv;
 }
 static const uint8_t fallback[]={MV_TACKLE,MV_SCRATCH,MV_POUND,MV_QUICK_ATTACK};
 for(uint8_t k=0;k<sizeof(fallback)&&w<3;k++){bool dup=false;for(uint8_t x=0;x<w;x++)if(out[x]==fallback[k])dup=true;if(!dup)out[w++]=fallback[k];}
 out[3]=digimonSignatureMove(i);
}
bool digimonMovesNeedRefresh(const uint8_t m[4]){
 uint8_t count=0,digital=0;
 for(uint8_t i=0;i<4;i++)if(m[i]){count++;if(m[i]>=MV_DIGI_PULSE)digital++;for(uint8_t j=0;j<i;j++)if(m[i]==m[j])return true;}
 return count<4||digital>1;
}
static uint16_t digiFind(uint8_t ver,const char*name){
 for(uint16_t i=0;i<DIGI_SPECIES_COUNT;i++)if(DIGI_SPECIES[i].version==ver&&!strcmp(DIGI_SPECIES[i].name,name))return i;
 return DIGI_NO_FUSION;
}
bool isPendulumFusionSpecies(uint16_t i){
 if(i>=DIGI_SPECIES_COUNT||DIGI_SPECIES[i].version<10)return false;
 const char*n=DIGI_SPECIES[i].name;
 return !strcmp(n,"Mastemon")||!strcmp(n,"Tlalocmon")||!strcmp(n,"Aegisdramon")||
        !strcmp(n,"Mitamamon")||!strcmp(n,"Voltobautamon")||!strcmp(n,"Cernumon")||
        !strcmp(n,"Omegamon")||!strcmp(n,"Chaosdramon")||!strcmp(n,"Proximamon");
}
const char *digimonDeviceLabel(uint8_t v){
 static const char*const labels[]={"P0 Virus Busters","P1 Nature Spirits","P2 Deep Savers",
   "P3 Nightmare Soldiers","P4 Wind Guardians","P5 Metal Empire"};
 if(v>=1&&v<=5){static const char*const dmc[]={"DMC Ver.1","DMC Ver.2","DMC Ver.3","DMC Ver.4","DMC Ver.5"};return dmc[v-1];}
 return v>=10&&v<=15?labels[v-10]:"융합";
}
static bool digiQualified(uint16_t material,uint16_t current,uint8_t lv,const uint8_t*best,uint8_t need=55){
 return material!=DIGI_NO_FUSION&&((material==current&&lv>=need)||(best&&best[material]>=need));
}
static uint16_t pendulumFusionTarget(uint16_t i,uint8_t lv,const uint8_t*best){
 if(i>=DIGI_SPECIES_COUNT)return DIGI_NO_FUSION;
 const uint8_t v=DIGI_SPECIES[i].version; const char*n=DIGI_SPECIES[i].name;
 auto q=[&](uint8_t ver,const char*name){return digiQualified(digiFind(ver,name),i,lv,best);};
 auto target=[&](const char*name){return digiFind(v,name);};
 if(v==11){
  if((!strcmp(n,"Angewomon")&&q(13,"LadyDevimon"))||(!strcmp(n,"LadyDevimon")&&q(11,"Angewomon")))return target("Mastemon");
  if((!strcmp(n,"SaberLeomon")&&(q(11,"ElDoradimon")||q(11,"MetalEtemon")))||
     ((!strcmp(n,"ElDoradimon")||!strcmp(n,"MetalEtemon"))&&q(11,"SaberLeomon")))return target("Tlalocmon");
 }else if(v==12){
  if((!strcmp(n,"Plesiomon")&&q(12,"MetalSeadramon"))||(!strcmp(n,"MetalSeadramon")&&q(12,"Plesiomon")))return target("Aegisdramon");
  if((!strcmp(n,"MarinAngemon")&&q(14,"Hououmon"))||(!strcmp(n,"Hououmon")&&q(12,"MarinAngemon")))return target("Mitamamon");
 }else if(v==13){
  if(!strcmp(n,"LadyDevimon")&&(q(11,"Angewomon")||q(10,"Angewomon")))return target("Mastemon");
  if((!strcmp(n,"Vamdemon")&&q(13,"Piemon"))||(!strcmp(n,"Piemon")&&q(13,"Vamdemon")))return target("Voltobautamon");
 }else if(v==14){
  if((!strcmp(n,"Hououmon")&&q(12,"MarinAngemon"))||(!strcmp(n,"MarinAngemon")&&q(14,"Hououmon")))return target("Mitamamon");
  if((!strcmp(n,"Griffomon")&&(q(14,"Pinochimon")||q(14,"Hydramon")))||
     (!strcmp(n,"Pinochimon")&&(q(14,"Griffomon")||q(14,"Hydramon")))||
     (!strcmp(n,"Hydramon")&&(q(14,"Pinochimon")||q(14,"Griffomon"))))return target("Cernumon");
 }else if(v==15){
  if((!strcmp(n,"WarGreymon")&&(q(15,"MetalGarurumon")||q(10,"MetalGarurumon")))||
     (!strcmp(n,"MetalGarurumon")&&(q(15,"WarGreymon")||q(10,"WarGreymon"))))return target("Omegamon");
  if((!strcmp(n,"Mugendramon")&&q(15,"HiAndromon"))||(!strcmp(n,"HiAndromon")&&q(15,"Mugendramon")))return target("Chaosdramon");
 }else if(v==10){
  if((!strcmp(n,"WarGreymon")&&(q(10,"MetalGarurumon")||q(15,"MetalGarurumon")))||
     (!strcmp(n,"MetalGarurumon")&&(q(10,"WarGreymon")||q(15,"WarGreymon"))))return target("Omegamon");
  if(!strcmp(n,"Angewomon")&&q(13,"LadyDevimon"))return target("Mastemon");
  if((!strcmp(n,"Siriusmon")&&q(10,"Arcturusmon"))||(!strcmp(n,"Arcturusmon")&&q(10,"Siriusmon")))return target("Proximamon");
 }
 return DIGI_NO_FUSION;
}
uint8_t digimonEvolutionLevel(uint16_t i){
  if(i==14||i==32||i==51||i==66||i==48||i==83)return 55;
  static const uint8_t n[]={2,5,12,25,45,100};return i<DIGI_SPECIES_COUNT?n[DIGI_SPECIES[i].stage]:100;
}
bool digimonHasEvolutionPotential(uint16_t i){
  if(i>=DIGI_SPECIES_COUNT)return false;
  if(isPendulumFusionSpecies(i))return false;
  if(DIGI_SPECIES[i].stage<DIGI_ULTIMATE)return true;
  return i==14||i==32||i==51||i==66||i==48||i==83||DIGI_SPECIES[i].version>=10;
}
static uint8_t digiStageRank(uint16_t i){
 if(i>=DIGI_SPECIES_COUNT)return 0;
 const DigiSpecies&cur=DIGI_SPECIES[i];uint8_t rank=0;
 for(uint16_t x=0;x<i;x++)if(DIGI_SPECIES[x].version==cur.version&&DIGI_SPECIES[x].stage==cur.stage)rank++;
 return rank;
}
static uint8_t digiStageCount(uint8_t version,uint8_t stage){
 uint8_t count=0;for(uint16_t x=0;x<DIGI_SPECIES_COUNT;x++)if(DIGI_SPECIES[x].version==version&&DIGI_SPECIES[x].stage==stage)count++;
 return count;
}
uint8_t digimonEvolutionBranches(uint16_t i,uint16_t out[4]){
 if(!out)return 0;for(uint8_t k=0;k<4;k++)out[k]=i;
 if(i>=DIGI_SPECIES_COUNT)return 0;const DigiSpecies&cur=DIGI_SPECIES[i];if(cur.stage>=DIGI_ULTIMATE)return 0;
 uint16_t all[12];uint8_t count=0;
 for(uint16_t x=0;x<DIGI_SPECIES_COUNT&&count<12;x++)if(DIGI_SPECIES[x].version==cur.version&&DIGI_SPECIES[x].stage==cur.stage+1&&!isPendulumFusionSpecies(x))all[count++]=x;
 if(!count)return 0;

 // DMC Ver.1-5 use compact, fixed late-stage families. Normally Adult ranks
 // 0-2, 3-5 and 6 evolve to Perfect ranks 0, 1 and 2. Ver.3 has four Perfects,
 // so it uses 2/2/2/1 to keep Chimairamon obtainable. Each Perfect then evolves
 // by rank; Ver.3's fourth Perfect shares its third Ultimate. Training style no
 // longer changes either result. Pendulum routes remain unchanged.
 if(cur.version>=1&&cur.version<=5&&(cur.stage==DIGI_ADULT||cur.stage==DIGI_PERFECT)){
  uint8_t rank=digiStageRank(i);
  uint8_t targetRank=cur.stage==DIGI_ADULT?(count==4?rank/2:rank/3):rank;
  if(targetRank>=count)targetRank=count-1;
  for(uint8_t stat=0;stat<4;stat++)out[stat]=all[targetRank];
  return 1;
 }

 // Every individual has a stable, small family instead of access to the whole
 // device roster. Babies may reach all available Child forms, while later
 // stages are limited to a local family of at most four/three evolutions.
 uint8_t limit=cur.stage<=DIGI_CHILD?4:(count<=3?2:3);if(limit>count)limit=count;
 uint8_t sourceCount=digiStageCount(cur.version,cur.stage);if(!sourceCount)sourceCount=1;
 uint8_t rank=digiStageRank(i);
 uint8_t start=sourceCount>1?(uint16_t)rank*(count-limit)/(sourceCount-1):0;

 for(uint8_t stat=0;stat<4;stat++){
  out[stat]=all[start+(stat%limit)];
 }
 return limit;
}
uint16_t digimonEvolutionTarget(uint16_t i,uint8_t lv,uint8_t a,uint8_t d,uint8_t s,uint8_t h,const uint8_t*best){
  if(i>=DIGI_SPECIES_COUNT)return i;
  if(best){
    if((i==14||i==32)&&(best[14]>=55||(i==14&&lv>=55))&&(best[32]>=55||(i==32&&lv>=55)))return DIGI_OMNIMON_ALTER_S;
    if((i==51||i==66)&&(best[51]>=55||(i==51&&lv>=55))&&(best[66]>=55||(i==66&&lv>=55)))return DIGI_CHAOSMON;
    if((i==48||i==83)&&(best[48]>=55||(i==48&&lv>=55))&&(best[83]>=55||(i==83&&lv>=55)))return DIGI_MILLENNIUMMON;
  }
  if(i==83&&lv>=60&&a>=80&&d>=60)return DIGI_CHAOSDRAMON;
  uint16_t pf=pendulumFusionTarget(i,lv,best);if(pf!=DIGI_NO_FUSION)return pf;
  const DigiSpecies&cur=DIGI_SPECIES[i];if(cur.stage>=DIGI_ULTIMATE||lv<digimonEvolutionLevel(i))return i;
  uint16_t branches[4];if(!digimonEvolutionBranches(i,branches))return i;
  uint16_t tr[4]={a,d,s,h};uint16_t high=tr[0];for(uint8_t x=1;x<4;x++)if(tr[x]>high)high=tr[x];
  uint8_t bestStat=0;for(uint8_t x=0;x<4;x++)if(tr[x]==high){bestStat=x;break;}
  return branches[bestStat];
}
