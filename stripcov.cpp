/*   Author: Uname(QingweiHe)
 *     Date: 2013/09/22
 
    读取LCOV输出的info文件, 根据stripcov自定义的配置文件格式屏蔽代码的行/函数/分支覆盖率
    输出新的info文件(默认打印到stdout, 一般应将该输出重定向到文件中或使用-o指定输出位置)
    配置文件格式如下:
    TOPDIR=topdir-path
    SOURCE_FILE_1:
        FUNC_TO_STRIP_11  #your comment here
        FUNC_TO_STRIP_12
        ...
    SOURCE_FILE_2:
        FUNC_TO_STRIP_21
        FUNC_TO_STRIP_22
        ...
    ...
    SOURCE_FILE_N:
        FUNC_TO_STRIP_N1
        FUNC_TO_STRIP_N2
    ...

    注意: topdir-path为SOURCE_FILE_1到SOURCE_FILE_N共有的目录名, 可写作根目录名/
 * */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <iostream>
#include <vector>
#include <map>
#include <set>

#define VERSION "1.0"

#define MAX_TOP_DIR_SIZE 512
#define MAX_LINE_SIZE 512
#define MAX_INFO_LINE_SIZE 1024
#define MAX_CFILE_SIZE 256
#define MAX_FUNC_NAME_SIZE 128
#define MAX_FUNC_LINE_NUM 0x7fffffff
#define MAX_NUM_LEN 10
#define TOP_DIR_FLAG "TOPDIR="
#define COMMENT_START_FLAG "#"
#define CFILE_STARTLINE_FLAG ':'
#define SF "SF:"
#define FN "FN:"
#define FNDA "FNDA:"
#define FNF "FNF:"
#define FNH "FNH:"
#define BRDA "BRDA:"
#define BRF "BRF:"
#define BRH "BRH:"
#define DA "DA:"
#define LF "LF:"
#define LH "LH:"
#define SF "SF:"
#define END_OF_RECORD "end_of_record"
#define LINE_IS(X) line_startswith(line, (X))
#define LP_COLON(LINE) strstr((LINE), ":")
#define LP_DOT(LINE) strstr((LINE), ",")

#if defined(__unix__)
#define REDSTR(STR)    "\033[31m"STR"\033[0m"
#define YELLOWSTR(STR) "\033[33m"STR"\033[0m"
#define GREENSTR(STR)  "\033[32m"STR"\033[0m"
#else
#define REDSTR(STR)    STR
#define YELLOWSTR(STR) STR
#define GREENSTR(STR)  STR
#endif

using namespace std;

typedef struct
{
    char *lcovinfo;  // -i,--info X   (Lcov输出的info文件)
    char *config;    // -c,--config X (配置文件)
    char *output;    // -o,--output X (屏蔽的输出info文件)
    char *topdir;    // -t,--topdir X (与配置文件中的TOPDIR定义相同, 优先使用该参数提供的路径)
    char rev_conf;   // -r,--revconf  (是否反转配置含义)
    char print_uncf; // -p,--print-uncf (打印未覆盖的函数)
    char dumpconf;   // -d,--dumpconf   (打印读取到内存的配置信息,用于调试)
    char quite;      // -q,--quite      (不要输出任何运行时信息)
}GlobalOptArgs;

static GlobalOptArgs g_optargs = {0};
static const char *optstr = "i:c:o:t:rpdqvh";
static const struct option longopts[] = {
    { "info",    required_argument, NULL, 'i' },
    { "config",  required_argument, NULL, 'c' },
    { "output",  required_argument, NULL, 'o' },
    { "topdir",  required_argument, NULL, 't' },
    { "revconf", no_argument,       NULL, 'r' },
    { "print-uncf",no_argument,     NULL, 'p' },
    { "dumpconf",no_argument,       NULL, 'd' },
    { "quite",   no_argument,       NULL, 'q' },
    { "version", no_argument,       NULL, 'v' },
    { "help",    no_argument,       NULL, 'h' },
    { NULL,      no_argument,       NULL, 0 }
};

typedef set<string> FuncSet;
typedef map<string, FuncSet> FuncSetMap;
typedef struct
{
    bool is_shield;
    int startln;
    int endln;
}FuncInfo;
typedef pair<string, FuncInfo*> FuncInfoPair;

FuncSetMap g_funcsetmap;
set<string> g_cfileset;
char g_topdir[MAX_TOP_DIR_SIZE];
size_t g_topdir_size;
char g_print_ncovered_funcs = 0;

static void show_version()
{
    printf("Stripcov version %s (Author: Apache. ShenZhen-China 2013-11-07.\n"
           "Copyright 1998-2013 Tencent Inc. All Rights Reserved.\n\n", VERSION );
    exit(1);
}
static void show_usage()
{
    printf("Usage: stripcov [OPTIONS]\n\n"
           "Use stripcov to strip unused functions' data in LCOV's info file\n"
           "You can gain a new info file whitch compatibles with LCOV's output\n\n"
           "Misc:\n"
           "  -h, --help                Print this help, then exit\n"
           "  -v, --version             Print version number, then exit\n"
           "  -q, --quite               Do not print progress messages\n\n"
           "Options:\n"
           "  -i, --info   FILENAME     Set LCOV's info file you willing to strip\n"
           "  -c, --config FILENAME     Set this stripcov tool's config file\n"
           "  -o, --output FILENAME     Write new info file to FILENAME(write to stdout if not set)\n"
           "  -t, --topdir DIR          Set the general path of all c source files\n"
           "                            You can also set it by TOPDIR in your config file\n"
           "                            But this argument effects first\n"
           "  -r, --revconf             Reverser your the funtions-list's meaning under the c source file\n"
           "                            Refer to the user manual\n"
           "  -p, --print-nf            Print out the uncovered functions' name\n\n" );
    exit(1);
}

inline bool is_cfile_startline(char *line, const size_t tailpos)
{
    return line[tailpos] == CFILE_STARTLINE_FLAG;
}

char *trimed_line(const char *line, const size_t size, const char endch)
{
    static char trline[MAX_LINE_SIZE] = {0};
    char *p = trline;
    if(p == NULL) {
        fprintf(stderr, "Memory not enough\n");
        exit(1);
    }
    while(*line != endch) {
        if(*line == ' ' || *line == '\t') {
            line++;
            continue;
        }
        *p++ = *line++;
    }
    *p = '\0';
    return trline;
}

bool is_space_line(char *line)
{
    bool yes = true;
    char *pcomment = NULL;
    if((pcomment = strstr(line, COMMENT_START_FLAG)) != NULL) {
        while(*pcomment != ' ' && *pcomment != '\t' && --pcomment > line);
        *(pcomment + 1) = '\0';
    }
    while(*line != '\0') {
        if(*line != ' ' && *line != '\t' && *line != '\n' && *line != '\r') {
            yes = false;
            break;
        }
        line++;
    }
    
    return yes;
}


bool line_startswith(const char *str, const char *pre)
{
    size_t lenpre = strlen(pre),
           lenstr = strlen(str);
           
    return lenstr < lenpre ? false : strncmp(pre, str, lenpre) == 0;
}

void dump_funcmap()
{
    FuncSetMap::iterator it;
    printf("=====CONFIG DUMP=====\n");
    for(it = g_funcsetmap.begin(); it != g_funcsetmap.end(); it++) {
        printf("CFILE: (%s)\n", (it->first).c_str());
        for(FuncSet::iterator _it = g_funcsetmap[it->first].begin(); _it != g_funcsetmap[it->first].end(); _it++) {
            printf(" FUNC: (%s)\n", (*_it).c_str());
        }
    }
}
void dump_cfile_set()
{
    printf("=====CFILE SET DUMP=====\n");
    for(set<string>::iterator it = g_cfileset.begin(); it != g_cfileset.end(); it++) {
        printf("CFILE: (%s)\n", (*it).c_str());
    }
}

void dump_func_set(set<string> *func_set)
{
    printf("=====FUNC SET DUMP=====\n");
    for(set<string>::iterator it = func_set->begin(); it != func_set->end(); it++) {
        printf("FUNC: (%s)\n", (*it).c_str());
    }
}
void dump_funcinfo(vector<FuncInfoPair*> pfuncinfo)
{
    vector<FuncInfoPair*>::iterator it = pfuncinfo.begin();
    for(; it != pfuncinfo.end(); it++) {
        cout << " Function: " << (*it)->first << endl
             << " IsShiled: " << (*it)->second->is_shield << endl
             << "LineRange: " << (*it)->second->startln << "\t" << (*it)->second->endln << endl
             << "------------------------------------" << endl;
    }
}

void get_topdir(char *line) {
    if(g_optargs.topdir) {
        size_t topdir_len = strlen(g_optargs.topdir);
        if(topdir_len < MAX_TOP_DIR_SIZE - 1) {
            strncpy(g_topdir, g_optargs.topdir, topdir_len);
            g_topdir[topdir_len] = '\n';
            g_topdir[topdir_len + 1] = '\0';
            return;
        }else {
            fprintf(stderr, REDSTR("The topdir is too long: %s\n\n"), g_optargs.topdir);
            exit(1);
        }
    }
    if(strlen(line) > MAX_TOP_DIR_SIZE) {
        fprintf(stderr, REDSTR("The topdir is too long: %s\n\n"), line);
        exit(1);
    }
    char *p = &line[strlen(TOP_DIR_FLAG)];
    char *tp = g_topdir;
    while(*p != '\0') {
        if(*p == ' ' || *p == '\t') {
            p++;
            continue;
        }
        *tp++ = *p++;
    };
    *tp = '\0';
}

int read_config(const char *config_file)
{
    FILE *fp = NULL;
    if((fp = fopen(config_file, "r")) == NULL) {
        fprintf(stderr, REDSTR("Fail to open config file: %s\n\n"), config_file);
        return -1;
    }
    char line[MAX_LINE_SIZE] = {0};
    bool cfile_start_flag = false;
    string cfile, func;
    size_t size = 0;
    
    if(fgets(line, sizeof(line), fp) == NULL){
        fprintf(stderr, "Config file is empty\n");
        return -1;
    }
    if(!line_startswith(line, TOP_DIR_FLAG) && g_optargs.topdir == NULL) {
        fprintf(stderr, REDSTR("Config file must starts with TOPDIR=\n")
                        REDSTR("   e.g. %s/home/mmogbuilder/src/\n")
                        GREENSTR("OR your can specify the topdir with -t option\n\n"), TOP_DIR_FLAG);
        return -1;
    }
    get_topdir(line);
    g_topdir_size = strlen(g_topdir);
    while(fgets(line, sizeof(line), fp) != NULL) {
        if(is_space_line(line)) {
            continue;
        }
        size = strlen(line) - 1;
        line[size] = '\0';
        if(is_cfile_startline(line, size - 1)) { // c file start line
            cfile_start_flag = true;
            cfile = string(trimed_line(line, size - 1, CFILE_STARTLINE_FLAG));
            g_cfileset.insert(cfile);
            g_funcsetmap.insert(FuncSetMap::value_type(cfile, FuncSet()));
        }else {
            if(!cfile_start_flag) {
                fprintf(stderr, YELLOWSTR("ignore config line: %s\n"), line);
                continue; // 忽略未定义文件就出现的函数行
            }
            func = string(trimed_line(line, size, 0));
            g_funcsetmap[cfile].insert(func);
        }
    }
    fclose(fp);
    
    return 0;
}

void clear_funcinfo(vector<FuncInfoPair*> *funcinfovec)
{
    vector<FuncInfoPair*>::iterator it = funcinfovec->begin();
    for(; it != funcinfovec->end(); it++) {
        delete (*it)->second;
    }
    funcinfovec->clear();
}

inline void cfile_from_SFline(char **pcfile, char *line)
{
    *pcfile = line + g_topdir_size + 2;
}

inline FuncInfo *funcinfo_from_FNline(char **pfunc, char *line)
{
    char numstr[MAX_NUM_LEN] = {0};
    char *pcolon = strstr(line, ":");
    char *pdot = strstr(line, ",");
    
    if(pdot == NULL || pdot - pcolon < 2) {
        printf(REDSTR("Lcov info file format error, aborted\n"));
        abort();
    }
    FuncInfo *pfuncinfo = new FuncInfo();
    pfuncinfo->is_shield = false;
    strncpy(numstr, pcolon + 1, pdot - pcolon - 1);
    pfuncinfo->startln = atoi(numstr);
    pfuncinfo->endln = MAX_FUNC_LINE_NUM;
    *pfunc = pdot + 1;
    
    return pfuncinfo;
}

void func_and_rantims_from_FNDAline(char **pfunc, int *rantimes, char *line)
{
    char numstr[MAX_NUM_LEN] = {0};
    char *pcolon = strstr(line, ":");
    char *pdot = strstr(line, ",");
    
    if(pdot == NULL || pdot - pcolon < 2) {
        printf(REDSTR("Lcov info file format error, aborted\n"));
        abort();
    }
    strncpy(numstr, pcolon + 1, pdot - pcolon - 1);
    *rantimes = atoi(numstr);
    *pfunc = pdot + 1;
}

inline int ivalue_from_line(char *line)
{
    char *numstr[MAX_NUM_LEN] = {0};
    char *pcolon = strstr(line, ":");

    return atoi(pcolon + 1);
}

void branch_info_from_BRDAline(int *brln_no, int *br_rantims, char *line)
{
    char *pcolon = LP_COLON(line);
    char *pdata = pcolon + 1;
    char *pdot = LP_DOT(pdata);
    if(pdot == NULL) {
        fprintf(stderr, REDSTR("Lcov info file format error, aborted(line: %s)\n"), line);
        abort();
    }
    char numstr[MAX_NUM_LEN] = {0};
    strncpy(numstr, pdata, pdot - pdata);
    *brln_no = atoi(numstr);
    char *p = pdot + 1;
    int dotcnt = 0;
    while(*p != '\0' && dotcnt < 2) {
        if(*p++ == ',') {
            dotcnt++;
        }
    }
    if(dotcnt != 2) {
        fprintf(stderr, REDSTR("Lcov info file format error, aborted(line: %s)\n"), line);
        abort();
    }
    *br_rantims = atoi(p + 1);
}

void codeline_info_from_DAline(int *codeln_no, int *code_rantimes, char *line)
{
    char *pcolon = LP_COLON(line);
    char *pdot = LP_DOT(pcolon + 1);
    
    if(pdot == NULL) {
        fprintf(stderr, REDSTR("Lcov info file format error, aborted(line: %s)\n"), line);
        abort();
    }
    char numstr[MAX_NUM_LEN] = {0};
    strncpy(numstr, pcolon + 1, pdot - pcolon);
    *codeln_no = atoi(numstr);
    *code_rantimes = atoi(pdot + 1);
}

inline bool is_brln_in_sheild_funcs(vector<FuncInfoPair*> *funcinfovec, const int brln_no)
{
    vector<FuncInfoPair*>::iterator it = funcinfovec->begin();
    for(; it != funcinfovec->end(); it++) {
        if(!(*it)->second->is_shield) {
            continue;
        }
        if(brln_no >= (*it)->second->startln && brln_no < (*it)->second->endln) {
            return true;
        }
    }
    return false;
}

inline bool is_codeline_in_sheild_funcs(vector<FuncInfoPair*> *funcinfovec, const int brln_no)
{
    return is_brln_in_sheild_funcs(funcinfovec, brln_no);
}

int parse_info_lines(FILE *const rfp, FILE *wfp)
{
    FILE *ifp = stdout;
    if(wfp == NULL) {
        wfp = stdout;
        ifp = stderr;
    }
    char line[MAX_INFO_LINE_SIZE] = {0};
    bool hit_stripfile = false;
    set<string> *pvec_shield_funcs = NULL;
    vector<FuncInfoPair*> funcinfovec;
    int ifnf, ifnh, ibrh, ibrf, ilf, ilh;
    char *pfunc, *pcfile;
    while(fgets(line, sizeof(line), rfp) != NULL) {
        line[strlen(line) - 1] = '\0';
        if(!hit_stripfile) {
            fprintf(wfp, "%s\n", line);
            if(!LINE_IS(SF)) {
                continue;
            }
            cfile_from_SFline(&pcfile, line);
            if(g_cfileset.find(string(pcfile)) == g_cfileset.end()) {
                continue;
            }
            g_optargs.quite == 1 && fprintf(ifp, "start to processing %s\n", pcfile);
            ifnf = ifnh = ibrh = ibrf = ilf = ilh = 0;
            hit_stripfile = true;
            pvec_shield_funcs = &g_funcsetmap[string(pcfile)];
#ifdef TESTDEBUG
            dump_func_set(pvec_shield_funcs);
#endif
            clear_funcinfo(&funcinfovec);
            
        }
        
        if(LINE_IS(FN)) {
            FuncInfo *funcinfo = funcinfo_from_FNline(&pfunc, line);
            FuncInfoPair *pfipair = new FuncInfoPair();
            pfipair->first = string(pfunc);
            pfipair->second = funcinfo;
            funcinfovec.push_back(pfipair);
            size_t vec_size = funcinfovec.size();
            if(vec_size > 1) {
                funcinfovec.at(vec_size - 2)->second->endln = funcinfo->startln;
            }
            
            if(g_optargs.rev_conf != 1 ? pvec_shield_funcs->find(string(pfunc)) != pvec_shield_funcs->end() :
                                        pvec_shield_funcs->find(string(pfunc)) == pvec_shield_funcs->end()) {
                funcinfovec.at(vec_size - 1)->second->is_shield = true;
            }else {
                fprintf(wfp, "%s\n", line);
            }
        }else if (LINE_IS(FNDA)) {
            int rantimes = 0;
            func_and_rantims_from_FNDAline(&pfunc, &rantimes, line);
            
            if(g_optargs.rev_conf != 1 ? pvec_shield_funcs->find(string(pfunc)) == pvec_shield_funcs->end() :
                                        pvec_shield_funcs->find(string(pfunc)) != pvec_shield_funcs->end() ) {
                fprintf(wfp, "%s\n", line); 
                if(rantimes == 0 && g_print_ncovered_funcs) {
                    fprintf(ifp, "\t%s\n", pfunc);
                } 
            } else {
                ifnf--;
                if(rantimes > 0) {
                    ifnh--;
                }
            }
        }else if (LINE_IS(FNF)) {
            int new_iFNF = ivalue_from_line(line) + ifnf;
            fprintf(wfp, FNF"%d\n", new_iFNF);
        }else if (LINE_IS(FNH)) {
            int new_iFNH = ivalue_from_line(line) + ifnh;
            fprintf(wfp, FNH"%d\n", new_iFNH);
        }else if (LINE_IS(BRDA)) {
            int brln_no, br_rantims;
            branch_info_from_BRDAline(&brln_no, &br_rantims, line);
            if(is_brln_in_sheild_funcs(&funcinfovec, brln_no)) {
                ibrf--;
                if(br_rantims > 0) {
                    ibrh--;
                }
            }else {
                fprintf(wfp, "%s\n", line);
            }
        }else if(LINE_IS(BRF)) {
            int new_iBRF = ivalue_from_line(line) + ibrf;
            fprintf(wfp, BRF"%d\n", new_iBRF);
        }else if(LINE_IS(BRH)) {
            int new_iBRH = ivalue_from_line(line) + ibrh;
            fprintf(wfp, BRH"%d\n", new_iBRH);
        }else if(LINE_IS(DA)) {
            int codeln_no, code_rantimes;
            codeline_info_from_DAline(&codeln_no, &code_rantimes, line);
            if(is_codeline_in_sheild_funcs(&funcinfovec, codeln_no)) {
                ilf--;
                if(code_rantimes > 0) {
                    ilh--;
                }
            }else {
                fprintf(wfp, "%s\n", line);
            }
        }else if(LINE_IS(LF)) {
            int new_iLF = ivalue_from_line(line) + ilf;
            fprintf(wfp, LF"%d\n", new_iLF);
        }else if(LINE_IS(LH)) {
            int new_iLH = ivalue_from_line(line) + ilh;
            fprintf(wfp, LH"%d\n", new_iLH);
        }else if(LINE_IS(END_OF_RECORD)) {
            hit_stripfile = false;
            fprintf(wfp, "%s\n", line);
        }else if(LINE_IS(SF)) {
            // do nothing...
        }else {
            fprintf(stderr, REDSTR("Unknown line: %s\n"), line);
        }
    }
    
    return 0;
}

static int get_opts(int argc, char *argv[])
{
    int opt;
    g_optargs.quite = 1;
    while((opt = getopt_long(argc, argv, optstr, longopts, NULL)) != -1) {
        switch(opt) {
        case 'i':
            g_optargs.lcovinfo = optarg;
            break;
        case 'c':
            g_optargs.config = optarg;
            break;
        case 'o':
            g_optargs.output = optarg;
            break;
        case 't':
            g_optargs.topdir = optarg;
            break;
        case 'r':
            g_optargs.rev_conf = 1;
            break;
        case 'p':
            g_optargs.print_uncf = 1;
            break;
        case 'q':
            g_optargs.quite = 0;
            break;
        case 'd':
            g_optargs.dumpconf = 1;
            break;
        case 'v':
            show_version();
        case 'h':
            show_usage();
        default:
            break;
        };
    }
    return (g_optargs.config!= NULL 
        && (g_optargs.lcovinfo != NULL || g_optargs.dumpconf != 0)) ? 0 : -1;
}

int main(int argc, char *argv[])
{
    
    if(get_opts(argc, argv) != 0) {
        show_usage();
    }

    if(read_config(g_optargs.config) != 0) {
        return -1;
    }
    
    if(g_optargs.dumpconf != 0) {
        dump_funcmap();
        dump_cfile_set();
        return 1;
    }
    
    g_print_ncovered_funcs = g_optargs.print_uncf;
    
    FILE *rfp = fopen(g_optargs.lcovinfo, "r");
    if(rfp == NULL) {
        fprintf(stderr, REDSTR("Fail to open LCOV's info file: %s\n\n"), g_optargs.lcovinfo);
        return -1;
    }
    FILE *wfp = NULL;
    if(g_optargs.output != NULL && (wfp = fopen(g_optargs.output, "w")) == NULL) {
        fprintf(stderr, REDSTR("Fail to open output file: %s\n\n"), g_optargs.output);
        return -1;
    }
    parse_info_lines(rfp, wfp);
    fclose(rfp);
    if(wfp) {
        fclose(wfp);
    }
    
    g_optargs.quite && fprintf(stderr, GREENSTR("done:)\n"));
    
    return 0;
}
