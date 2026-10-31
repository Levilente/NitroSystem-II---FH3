// Host test for the nitrous kit upgrade layer (nu_upgrade.h): fake platform, fake game memory, fake ini.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <string>
#include "windows.h"
#include "fh2mod.h"

size_t VirtualQuery(const void*p,MEMORY_BASIC_INFORMATION*m,size_t){
    m->BaseAddress=(void*)((uintptr_t)p&~(uintptr_t)0xFFF);m->State=MEM_COMMIT;m->Protect=PAGE_READWRITE;
    m->RegionSize=(size_t)0x40000000;return sizeof *m;}
BOOL CloseHandle(HANDLE){return 1;}
LONG InterlockedIncrement(volatile LONG*p){return ++*p;}
LONG InterlockedExchangeAdd(volatile LONG*p,LONG d){LONG o=*p;*p+=d;return o;}
void Sleep(DWORD){}
HANDLE GetProcessHeap(void){return (HANDLE)1;}
void *HeapAlloc(HANDLE,DWORD,size_t n){return malloc(n);}
BOOL HeapFree(HANDLE,DWORD,void*p){free(p);return 1;}
static DWORD (WINAPI *worker)(LPVOID);
HANDLE CreateThread(void*,size_t,DWORD(WINAPI*f)(LPVOID),void*,DWORD,DWORD*){worker=f;return (HANDLE)1;}
static uint64_t now_ms=100000;
uint64_t GetTickCount64(void){return now_ms;}
// fake ini: "section/key" -> text
static std::map<std::string,std::wstring> ini;
static std::string narrow(const wchar_t*w){std::string s;for(;*w;w++)s+=(char)*w;return s;}
static std::string ik(const wchar_t*s,const wchar_t*k){return narrow(s)+"/"+narrow(k);}
unsigned GetPrivateProfileIntW(LPCWSTR s,LPCWSTR k,int d,LPCWSTR){auto i=ini.find(ik(s,k));return i==ini.end()?(unsigned)d:(unsigned)wcstol(i->second.c_str(),0,10);}
DWORD GetPrivateProfileStringW(LPCWSTR s,LPCWSTR k,LPCWSTR d,LPWSTR out,DWORD n,LPCWSTR){auto i=ini.find(ik(s,k));const wchar_t*v=i==ini.end()?d:i->second.c_str();wcsncpy(out,v,n-1);out[n-1]=0;return (DWORD)wcslen(out);}
BOOL WritePrivateProfileStringW(LPCWSTR s,LPCWSTR k,LPCWSTR v,LPCWSTR){if(!v)ini.erase(ik(s,k));else ini[ik(s,k)]=v;return 1;}

static char logbuf[400][900]; static int nlog;
static void nu_say(const char *t){if(nlog<400)strncpy(logbuf[nlog++],t,899);printf("  log: %.150s\n",t);}
#include "nu_upgrade.h"

static FH2Mod *f_register(const FH2ModInfo*){return (FH2Mod*)1;}
static void f_log(FH2Mod*,const char*){}
static unsigned char *img; static void *hooked_fn,*detour; static void **orig; static int enabled,hooks_made;
static void *f_wait(uint32_t){return img;}
static unsigned fake_getter_value=24; static int getter_calls;
static unsigned fake_getter(void*){getter_calls++;return fake_getter_value;}
static const uint16_t empty_text[1]={0}; static const uint16_t real_text[3]={'H','i',0};
static const uint16_t *fake_lookup(void*,unsigned,unsigned k){return k==0x1234?real_text:empty_text;}
static FH2Hook *f_hf(FH2Mod*,const char*n,void*site,void*det,void**o){
    if(strstr(n,"database query")){hooked_fn=site;detour=det;orig=o;hooks_made++;return (FH2Hook*)1;}
    if(strstr(n,"string lookup")){assert(det==(void*)nu_lookup_hook&&site==img+NU_RVA_LOC_LOOKUP);*o=(void*)fake_lookup;hooks_made++;return (FH2Hook*)6;}
    if(strstr(n,"garage id")){assert(det==(void*)nu_getter_hook);*o=(void*)fake_getter;hooks_made++;return (FH2Hook*)8;}
    assert(0);return NULL;}
static int f_en(FH2Mod*,FH2Hook*h){assert(h!=NULL);enabled++;return 1;}
static std::string added_sql; static int sql_calls;
static void f_sql(FH2Mod*,const char*s){sql_calls++;added_sql=s;}
static std::map<std::string,unsigned> set_files;
static int f_setfile(FH2Mod*,const char*path,const void *data,size_t size){set_files[path]=(unsigned)size;assert(data&&size>0x53&&!memcmp(data,"burG",4));return 1;}
static char last_sql[200000]; static int real_calls;
static void *real_fn(void*,void**,const char*sql){if(sql)strcpy(last_sql,sql);real_calls++;return (void*)3;}

int main(void){
    img=(unsigned char*)calloc(1,0x4000000);
    void **vt=(void**)calloc(16,sizeof(void*)); vt[NU_DB_SLOT]=(void*)(img+0x2F0000); memset(img+0x2F0000,0x90,32);
    static FH2ModApi a;a.size=sizeof a;a.api_version=9;a.register_mod=f_register;a.log=f_log;
    a.wait_for_code=f_wait;a.hook_function=f_hf;a.hook_call=NULL;a.disable_hooks=NULL;a.enable_hooks=f_en;a.add_sql=f_sql;a.set_file=f_setfile;
    const wchar_t *ini_path=L"C:\\mods\\FnFNitro.ini";
    FH2ModApi old=a;old.api_version=2;assert(nu_init(&old,(FH2Mod*)1,ini_path,5)==0);
    FH2ModApi nosql=a;nosql.add_sql=NULL;assert(nu_init(&nosql,(FH2Mod*)1,ini_path,5)==0);
    assert(sql_calls==0);
    /* a first start writes the defaults into the ini so they can be edited */
    assert(nu_init(&a,(FH2Mod*)1,ini_path,5)==1&&worker&&sql_calls==1);
    /* the custom upgrade icons: each one is handed over once under the folder of the game's Upgrade_Parts.zip, and the database rows name the same file */
    assert(set_files.size()==(size_t)ICON_COUNT&&ICON_COUNT>0);
    for(auto&kv:set_files){assert(kv.first.compare(0,38,"ui/textures/data_bound/Upgrade_Parts/C")==0&&kv.second==65619u);
        std::string f=kv.first.substr(kv.first.rfind('/')+1);assert(added_sql.find("Custom\\"+f)!=std::string::npos);}
    assert(GetPrivateProfileIntW(L"Upgrade",L"Price1",0,0)==3000&&GetPrivateProfileIntW(L"Upgrade",L"Price3",0,0)==12000&&GetPrivateProfileIntW(L"Upgrade",L"Area",0,0)==1);
    assert(ini["Upgrade/Boost1"]==L"1.2"&&ini["Upgrade/Boost2"]==L"1.4"&&ini["Upgrade/Boost3"]==L"1.6");
    assert(nu_cfg.tenths[1]==12&&nu_cfg.tenths[3]==16&&nu_boost(0)==1.0f&&nu_boost(2)>1.39f&&nu_boost(2)<1.41f&&nu_boost(9)>1.59f);
    assert(added_sql.find("INSERT OR REPLACE INTO UpgradeTypes")!=std::string::npos&&added_sql.find("'_&2119673024'")!=std::string::npos);   /* 0x7E57A4C0 */
    assert(added_sql.find("List_UpgradeMotorParts")!=std::string::npos&&added_sql.find("900473, -7452, 3, 0, 0, 0.0, 0.0, 12000, 1.0")!=std::string::npos);
    /* edited values are used (and clamped) */
    ini[ "Upgrade/Price2"]=L"8000"; ini["Upgrade/Boost3"]=L"1.85"; ini["Upgrade/Boost1"]=L"9"; ini["Upgrade/Area"]=L"4";
    nu_load_config(ini_path,5);
    assert(nu_cfg.price[2]==8000&&nu_cfg.tenths[3]==18&&nu_cfg.tenths[1]==30&&nu_cfg.area==4);
    ini["Upgrade/Area"]=L"77"; ini["Upgrade/Boost1"]=L"abc"; ini["Upgrade/Price1"]=L"-5";
    nu_load_config(ini_path,5);
    assert(nu_cfg.area==1&&nu_cfg.tenths[1]==12&&nu_cfg.price[1]==3000);
    ini["Upgrade/Price2"]=L"6000"; ini["Upgrade/Boost3"]=L"1.6"; ini.erase("Upgrade/Boost1"); nu_load_config(ini_path,5);

    /* the game code the hooks check for */
    memcpy(img+NU_RVA_LOC_LOOKUP,"\x48\x83\xEC\x38\xB8\xFF\xFF\x00\x00\x45\x8B\xC8\x66\x3B\xD0",15);
    memcpy(img+NU_RVA_CURRENT_GARAGE,"\x40\x53\x48\x83\xEC\x40\x48\xC7\x44\x24\x20\xFE\xFF\xFF\xFF\x4C\x8D\x05",18);
    unsigned char *obj=(unsigned char*)calloc(1,64);*(void***)obj=vt;*(void**)(img+NU_RVA_DB_GLOBAL)=obj;
    worker(NULL);
    assert(hooked_fn==vt[NU_DB_SLOT]&&hooks_made==3&&enabled==3&&detour==(void*)nu_query_hook);
    assert(nu_read_current_garage()==0);   /* nothing sensible in the fake image: no call, no crash */
    *orig=(void*)real_fn;
    void *(*q)(void*,void**,const char*)=(void*(*)(void*,void**,const char*))detour;
    void *out=NULL;

    /* other statements pass unchanged */
    assert(q(obj,&out,"SELECT 1 FROM Data_Car WHERE Id = 5 AND something longer")==(void*)3&&strcmp(last_sql,"SELECT 1 FROM Data_Car WHERE Id = 5 AND something longer")==0);
    assert(q(obj,&out,NULL)==(void*)3&&real_calls==2);

    /* the view of car 2049 gets the nitrous kit, before the first existing UNION; a second pass adds nothing */
    const char *view="CREATE VIEW Profile0_View_UpgradeParts AS SELECT 0 AS PartEnum, Id AS PartId, Ordinal, IsStock, Level, ManufacturerID, Price, 0 AS IsException FROM List_UpgradeEngine WHERE Ordinal = 2049 UNION SELECT 1 AS PartEnum, Id AS PartId, Ordinal, IsStock, Level, ManufacturerID, Price, 0 AS IsException FROM List_UpgradeDrivetrain WHERE Ordinal = 2049";
    q(obj,&out,view);
    assert(strstr(last_sql,"900470 AS PartId, 2049 AS Ordinal")&&strstr(last_sql,"900473 AS PartId, 2049 AS Ordinal, 0 AS IsStock, 3 AS Level, NULL AS ManufacturerID, 12000 AS Price")&&strstr(last_sql,"FROM List_UpgradeDrivetrain")&&nu_cur_ord==2049);
    assert(strstr(last_sql,"List_UpgradeEngine WHERE Ordinal = 2049 UNION SELECT 47"));
    std::string once=last_sql; q(obj,&out,once.c_str()); assert(once==last_sql);
    /* Universal Wheel Spacers' own rows in the view (if it ran first) are kept */
    { std::string v2=std::string(view); size_t u=v2.find(" UNION SELECT "); v2.insert(u," UNION SELECT 45 AS PartEnum, 900450 AS PartId, 2049 AS Ordinal, 1 AS IsStock, 0 AS Level, NULL AS ManufacturerID, 0 AS Price, 0 AS IsException");
      q(obj,&out,v2.c_str()); assert(strstr(last_sql,"900450 AS PartId")&&strstr(last_sql,"900470 AS PartId")); }

    /* part list: nothing installed -> stock; the tire part list (enum 8) is not touched */
    const char *list="SELECT PartTable.PartId AS id,  Upgrades.Name AS Name,  CASE    WHEN PartTable.PartId = 2049005 THEN '1'    WHEN PartTable.IsStock OR EXISTS(      SELECT PartId FROM Profile0_Career_PurchasedParts AS purchasedParts      WHERE purchasedParts.GarageId = 27        AND purchasedParts.PartEnum = UpgradeTypes.PartEnum        AND purchasedParts.PartId = PartTable.PartId) THEN '2'    ELSE '0'  END AS Status  FROM Upgrades WHERE PartTable.PartEnum = 47 ORDER BY PartTable.IsStock DESC";
    q(obj,&out,list); assert(strstr(last_sql,"WHEN PartTable.PartId = 900470 THEN '1'"));
    std::string tire=list; tire.replace(tire.find("PartEnum = 47"),13,"PartEnum = 8 "); q(obj,&out,tire.c_str()); assert(tire==last_sql);
    /* the game installs stage 2 on garage 27: the write is swallowed, the choice kept, the list follows */
    assert(nu_level()==0);
    nu_active_garage=27;
    q(obj,&out,"UPDATE Profile0_Career_Garage SET MotorParts = 900472 WHERE Id = 27");
    assert(strcmp(last_sql,"UPDATE Profile0_Career_Garage SET MotorParts = MotorParts WHERE Id = 27")==0);
    assert(nu_get_level(27,2049)==2&&nu_get_level(28,2049)==0&&nu_level()==2);          /* the driven car got it at once */
    assert(GetPrivateProfileIntW(L"GarageNitro",L"27",0,0)==2&&GetPrivateProfileIntW(L"GarageCar",L"27",0,0)==2049);
    q(obj,&out,list); assert(strstr(last_sql,"WHEN PartTable.PartId = 900472 THEN '1'"));
    /* a purchase for a car that is not the driven one leaves the driven car's stage alone */
    q(obj,&out,"UPDATE Profile0_Career_Garage SET MotorParts = 900473 WHERE Id = 30");
    assert(nu_level()==2&&nu_get_level(30,2049)==3);
    /* spacers, tires, unrelated MotorParts are not touched */
    q(obj,&out,"UPDATE Profile0_Career_Garage SET MotorParts = 900453 WHERE Id = 27"); assert(strstr(last_sql,"MotorParts = 900453"));
    q(obj,&out,"UPDATE Profile0_Career_Garage SET MotorParts = 77 WHERE Id = 27"); assert(strstr(last_sql,"MotorParts = 77"));
    q(obj,&out,"UPDATE Profile0_Career_Garage SET TireCompound = 2049001 WHERE Id = 27"); assert(strstr(last_sql,"TireCompound = 2049001"));
    /* the stock entry counts as bought, the real stages do not get that */
    q(obj,&out,"SELECT 1 From Profile0_Career_PurchasedParts WHERE GarageId = 27 AND PartEnum = 47 AND PartId = 900470 AND ManufacturerId = -1");
    assert(strcmp(last_sql,"SELECT 1")==0);
    q(obj,&out,"SELECT 1 From Profile0_Career_PurchasedParts WHERE GarageId = 27 AND PartEnum = 47 AND PartId = 900472 AND ManufacturerId = -1");
    assert(strstr(last_sql,"PartId = 900472"));
    q(obj,&out,"SELECT 1 From Profile0_Career_PurchasedParts WHERE GarageId = 27 AND PartEnum = 45 AND PartId = 900470 AND ManufacturerId = -1");
    assert(strstr(last_sql,"PartEnum = 45"));
    /* back to stock */
    q(obj,&out,"UPDATE Profile0_Career_Garage SET MotorParts = 900470 WHERE Id = 27");
    assert(nu_level()==0&&nu_get_level(27,2049)==0);
    q(obj,&out,"UPDATE Profile0_Career_Garage SET MotorParts = 900471 WHERE Id = 27"); assert(nu_level()==1);

    /* a garage id reused by another model (the car was sold): the saved stage is forgotten, also for the driven car */
    assert(nu_get_level(27,2384)==0&&nu_level()==0&&GetPrivateProfileIntW(L"GarageNitro",L"27",9,0)==0&&GetPrivateProfileIntW(L"GarageCar",L"27",0,0)==2384);
    /* ring buffer: many rewrites do not break */
    for(int i=0;i<100;i++)q(obj,&out,view);

    /* the garage id is recorded when the game's own getter runs; the stage of that car is read from the ini once */
    ini["GarageNitro/55"]=L"3"; ini["GarageCar/55"]=L"2049";
    nu_real_garage_getter=NULL; assert(nu_getter_hook(NULL)==0);
    nu_real_garage_getter=fake_getter; fake_getter_value=55;
    assert(nu_getter_hook(NULL)==55&&nu_active_garage==55&&nu_level()==3);
    nu_set_level(55,2049,1); assert(nu_level()==1);
    fake_getter_value=27; assert(nu_getter_hook(NULL)==27&&nu_level()==0);
    /* refresh on the database thread: nothing when the car is known and unchanged; 2 s apart when unknown / changed */
    getter_calls=0; nu_garage_dirty=0; now_ms+=60000; nu_refresh_garage(); assert(getter_calls==0&&nu_last_garage_ms!=now_ms);
    nu_mark_dirty(); now_ms+=500; nu_last_garage_ms=now_ms-100; nu_refresh_garage(); assert(nu_garage_dirty==1);   /* too soon */
    now_ms+=2500; nu_refresh_garage();   /* the fake image has no real profile chain: nothing read, dirty cleared */
    assert(!nu_garage_dirty);
    /* another mod hooked a function first: its jump at the start still counts as the game code */
    { unsigned char *p=img+NU_RVA_LOC_LOOKUP,keep=p[0]; static const unsigned char exp[2]={0x48,0x83};
      p[0]=0xE9; assert(nu_code_matches(NU_RVA_LOC_LOOKUP,exp,2)); p[0]=0x11; assert(!nu_code_matches(NU_RVA_LOC_LOOKUP,exp,2)); p[0]=keep; }
    /* our texts: the game's lookup finds nothing for our keys, we answer; other keys are untouched */
    { const uint16_t *t=nu_lookup_hook(NULL,5,0xA4C0); assert(t[0]=='N'&&t[8]=='K');             /* Nitrous Kit */
      t=nu_lookup_hook(NULL,5,0xA4C8); assert(t[0]=='S'&&t[4]=='k'&&t[5]==0);                    /* Stock */
      t=nu_lookup_hook(NULL,5,0xA4CA); assert(t[0]=='S'&&t[6]=='2');                             /* Stage 2 Nitrous */
      t=nu_lookup_hook(NULL,5,0xA4D3); assert(t[0]=='N'&&t[15]=='3'&&t[18]=='1'&&t[19]=='.'&&t[20]=='6');    /* "Nitrous, stage 3: 1.6x ..." */
      assert(nu_lookup_hook(NULL,5,0x1234)==real_text&&nu_lookup_hook(NULL,5,0x4321)==empty_text);
      assert(nu_lookup_hook(NULL,5,0xA470)==empty_text&&nu_lookup_hook(NULL,5,0xA4C4)==empty_text);   /* Wheel Spacers' key / unused key */
    }
    nu_real_query=NULL; assert(nu_query_hook(obj,&out,"x")==NULL);
    printf("OK: %d log lines\n",nlog);
    return 0;
}
