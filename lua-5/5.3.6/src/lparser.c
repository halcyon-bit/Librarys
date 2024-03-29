﻿/*
** $Id: lparser.c,v 2.155.1.2 2017/04/29 18:11:40 roberto Exp $
** Lua Parser
** See Copyright Notice in lua.h
*/

#define lparser_c
#define LUA_CORE

#include "lprefix.h"


#include <string.h>

#include "lua.h"

#include "lcode.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "llex.h"
#include "lmem.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lparser.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"

/* maximum number of local variables per function (must be smaller
   than 250, due to the bytecode format) */
#define MAXVARS		200


#define hasmultret(k)		((k) == VCALL || (k) == VVARARG)


   /* because all strings are unified by the scanner, the parser
      can use pointer equality for string equality */
#define eqstr(a,b)	((a) == (b))


      /*
      ** nodes for block list (list of active blocks)
      */
typedef struct BlockCnt {
    struct BlockCnt* previous;  /* chain */
    int firstlabel;  /* index of first label in this block */
    int firstgoto;  /* index of first pending goto in this block */
    lu_byte nactvar;  /* # active locals outside the block */
    lu_byte upval;  /* true if some variable in the block is an upvalue */
    lu_byte isloop;  /* true if 'block' is a loop */
} BlockCnt;



/*
** prototypes for recursive non-terminal functions
*/
static void statement(LexState* ls);
static void expr(LexState* ls, expdesc* v);


/* semantic error */
static l_noret semerror(LexState* ls, const char* msg) {
    ls->t.token = 0;  /* remove "near <token>" from final message */
    luaX_syntaxerror(ls, msg);
}


static l_noret error_expected(LexState* ls, int token) {
    luaX_syntaxerror(ls,
        luaO_pushfstring(ls->L, "%s expected", luaX_token2str(ls, token)));
}


static l_noret errorlimit(FuncState* fs, int limit, const char* what) {
    lua_State* L = fs->ls->L;
    const char* msg;
    int line = fs->f->linedefined;
    const char* where = (line == 0)
        ? "main function"
        : luaO_pushfstring(L, "function at line %d", line);
    msg = luaO_pushfstring(L, "too many %s (limit is %d) in %s",
        what, limit, where);
    luaX_syntaxerror(fs->ls, msg);
}


static void checklimit(FuncState* fs, int v, int l, const char* what) {
    if (v > l) errorlimit(fs, l, what);
}


static int testnext(LexState* ls, int c) {
    if (ls->t.token == c) {
        luaX_next(ls);
        return 1;
    }
    else return 0;
}


static void check(LexState* ls, int c) {
    if (ls->t.token != c)
        error_expected(ls, c);
}


static void checknext(LexState* ls, int c) {
    check(ls, c);
    luaX_next(ls);
}


#define check_condition(ls,c,msg)	{ if (!(c)) luaX_syntaxerror(ls, msg); }



static void check_match(LexState* ls, int what, int who, int where) {
    if (!testnext(ls, what)) {
        if (where == ls->linenumber)
            error_expected(ls, what);
        else {
            luaX_syntaxerror(ls, luaO_pushfstring(ls->L,
                "%s expected (to close %s at line %d)",
                luaX_token2str(ls, what), luaX_token2str(ls, who), where));
        }
    }
}


static TString* str_checkname(LexState* ls) {
    TString* ts;
    check(ls, TK_NAME);
    ts = ls->t.seminfo.ts;
    luaX_next(ls);
    return ts;
}

/*
** 初始化 expdesc, u.info = i
*/
static void init_exp(expdesc* e, expkind k, int i)
{
    e->f = e->t = NO_JUMP;
    e->k = k;
    e->u.info = i;
}

/* 创建常量(s), 并设置表达式为 VK, info 为常量表中的id */
static void codestring(LexState* ls, expdesc* e, TString* s)
{
    init_exp(e, VK, luaK_stringK(ls->fs, s));
}


static void checkname(LexState* ls, expdesc* e) {
    codestring(ls, e, str_checkname(ls));
}

/*
** 创建局部变量(varname), 并返回变量的id
*/
static int registerlocalvar(LexState* ls, TString* varname)
{
    FuncState* fs = ls->fs;
    Proto* f = fs->f;
    int oldsize = f->sizelocvars;
    luaM_growvector(ls->L, f->locvars, fs->nlocvars, f->sizelocvars,
        LocVar, SHRT_MAX, "local variables");
    while (oldsize < f->sizelocvars)
        f->locvars[oldsize++].varname = NULL;
    f->locvars[fs->nlocvars].varname = varname;
    luaC_objbarrier(ls->L, f, varname);
    return fs->nlocvars++;
}

/*
** 新建局部变量(name), 实际调用registerlocalvar(), 并将id保存在全局数据表内
*/
static void new_localvar(LexState* ls, TString* name)
{
    FuncState* fs = ls->fs;
    Dyndata* dyd = ls->dyd;
    int reg = registerlocalvar(ls, name);
    checklimit(fs, dyd->actvar.n + 1 - fs->firstlocal,
        MAXVARS, "local variables");
    luaM_growvector(ls->L, dyd->actvar.arr, dyd->actvar.n + 1,
        dyd->actvar.size, Vardesc, MAX_INT, "local variables");
    dyd->actvar.arr[dyd->actvar.n++].idx = cast(short, reg);
}

/*
** 新建字符串, 并将字符串作为变量名, 创建局部变量
*/
static void new_localvarliteral_(LexState* ls, const char* name, size_t sz)
{
    new_localvar(ls, luaX_newstring(ls, name, sz));
}

#define new_localvarliteral(ls,v) \
	new_localvarliteral_(ls, "" v, (sizeof(v)/sizeof(char))-1)

/* 获取当前函数fs作用域内第i个变量的信息(局部变量) */
static LocVar* getlocvar(FuncState* fs, int i)
{
    int idx = fs->ls->dyd->actvar.arr[fs->firstlocal + i].idx;
    lua_assert(idx < fs->nlocvars);
    return &fs->f->locvars[idx];
}

/*
** 调整有效变量的个数, 和有效起始位置(pc)
** nvars 为需要调整的变量的个数
*/
static void adjustlocalvars(LexState* ls, int nvars)
{
    FuncState* fs = ls->fs;
    fs->nactvar = cast_byte(fs->nactvar + nvars);
    for (; nvars; nvars--)
    {
        getlocvar(fs, fs->nactvar - nvars)->startpc = fs->pc;
    }
}

/*
** 移除无效的变量, 并设置变量的失效位置(pc)
** tolevel为现在最后一个有效变量的位置, 同时调整全局变量表的个数
** fs->nactvar - tolevel为无效变量的个数
** 仅在 leaveblock 中调用
*/
static void removevars(FuncState* fs, int tolevel)
{
    fs->ls->dyd->actvar.n -= (fs->nactvar - tolevel);
    while (fs->nactvar > tolevel)
        getlocvar(fs, --fs->nactvar)->endpc = fs->pc;
}

/*
**     在当前函数fs作用域内的所有 upvalue 中寻找字符串 name 的位置, 若找到返回在当前
** 函数体内的位置, 否则返回-1
*/
static int searchupvalue(FuncState* fs, TString* name)
{
    int i;
    Upvaldesc* up = fs->f->upvalues;
    for (i = 0; i < fs->nups; i++)
    {
        if (eqstr(up[i].name, name))
            return i;
    }
    return -1;  /* 没有找到 */
}

/*
** 新建 upvalue, 返回upvalue对应的id
*/
static int newupvalue(FuncState* fs, TString* name, expdesc* v)
{
    Proto* f = fs->f;
    int oldsize = f->sizeupvalues;
    checklimit(fs, fs->nups + 1, MAXUPVAL, "upvalues");
    /* 增长空间 */
    luaM_growvector(fs->ls->L, f->upvalues, fs->nups, f->sizeupvalues,
        Upvaldesc, MAXUPVAL, "upvalues");
    while (oldsize < f->sizeupvalues)
        f->upvalues[oldsize++].name = NULL;
    /*
    **     判断该变量是否在栈中, 如果是上层函数的local var, 则为1, 若是上层函数的
    ** upvalue, 则为0.
    */
    f->upvalues[fs->nups].instack = (v->k == VLOCAL);
    /* idx 为变量在外层函数的位置 */
    f->upvalues[fs->nups].idx = cast_byte(v->u.info);
    f->upvalues[fs->nups].name = name;
    luaC_objbarrier(fs->ls->L, f, name);
    return fs->nups++;  /* 返回 upvalue 的id */
}

/*
**     在当前函数fs作用域内的所有局部变量中寻找字符串n的位置, 若找到返回在当前函
** 数体内的位置, 否则返回-1
*/
static int searchvar(FuncState* fs, TString* n)
{
    int i;
    for (i = cast_int(fs->nactvar) - 1; i >= 0; i--)
    {
        if (eqstr(n, getlocvar(fs, i)->varname))
            return i;
    }
    return -1;  /* 没找到 */
}


/*
** 找到 upvalue 所属的 block, 标记该 block 中有变量成为 upvalue
** 之后会发出关闭指令
*/
static void markupval(FuncState* fs, int level)
{
    BlockCnt* bl = fs->bl;
    while (bl->nactvar > level)
        bl = bl->previous;
    bl->upval = 1;
}


/*
**     查找名称为"n"的变量, 如果 n 是 upvalue, 则将其加入到所有的中间函数的
** upvalue 列表.
*/
static void singlevaraux(FuncState* fs, TString* n, expdesc* var, int base)
{
    if (fs == NULL)  /* 最顶层? */
        init_exp(var, VVOID, 0);  /* 则表明是个全局变量 */
    else
    {
        int v = searchvar(fs, n);  /* 在当前函数作用域的局部变量中查找 */
        if (v >= 0)
        {   /* 找到了? */
            init_exp(var, VLOCAL, v);  /* 变量为局部变量, 设置表达式 */
            if (!base)  /* 递归调用时成立, 表明该变量是个upvalue */
                markupval(fs, v);  /* 标记该变量被当做 upvalue 使用 */
        }
        else
        {   /* 在当前函数的局部变量中没有找到; 尝试在 upvalues 中查找 */
            int idx = searchupvalue(fs, n);  /* 尝试在 upvalues 中查找 */
            if (idx < 0)
            {   /* 没有找到? */
                singlevaraux(fs->prev, n, var, 0);  /* 尝试在上一层的函数中查找 */
                if (var->k == VVOID)  /* 没有找到? */
                    return;  /* 则是个全局变量 */
                /* else was LOCAL or UPVAL */
                idx = newupvalue(fs, n, var);  /* 新建 upvalue */
            }
            init_exp(var, VUPVAL, idx);  /* 设置表达式, 变量为upvalue */
        }
    }
}

/*
** 对变量的查找
*/
static void singlevar(LexState* ls, expdesc* var)
{
    TString* varname = str_checkname(ls);  /* 获取变量名 */
    FuncState* fs = ls->fs;
    singlevaraux(fs, varname, var, 1);
    if (var->k == VVOID)
    {   /* 在所有函数中没有找到? 则是全局变量 */
        expdesc key;
        singlevaraux(fs, ls->envn, var, 1);  /* 获取环境变量 "__ENV" */
        lua_assert(var->k != VVOID);  /* 环境变量一定存在 */
        codestring(ls, &key, varname);  /* 创建常量, key 为 VK */
        luaK_indexed(fs, var, &key);  /* env[varname], var 为 VUPVAL */
    }
}


static void adjust_assign(LexState* ls, int nvars, int nexps, expdesc* e)
{
    FuncState* fs = ls->fs;
    int extra = nvars - nexps;
    if (hasmultret(e->k)) {
        extra++;  /* includes call itself */
        if (extra < 0) extra = 0;
        luaK_setreturns(fs, e, extra);  /* last exp. provides the difference */
        if (extra > 1) luaK_reserveregs(fs, extra - 1);
    }
    else {
        if (e->k != VVOID) luaK_exp2nextreg(fs, e);  /* close last expression */
        if (extra > 0) {
            int reg = fs->freereg;
            luaK_reserveregs(fs, extra);
            luaK_nil(fs, reg, extra);
        }
    }
    if (nexps > nvars)
        ls->fs->freereg -= nexps - nvars;  /* remove extra values */
}


static void enterlevel(LexState* ls) {
    lua_State* L = ls->L;
    ++L->nCcalls;
    checklimit(ls->fs, L->nCcalls, LUAI_MAXCCALLS, "C levels");
}


#define leavelevel(ls)	((ls)->L->nCcalls--)


static void closegoto(LexState* ls, int g, Labeldesc* label) {
    int i;
    FuncState* fs = ls->fs;
    Labellist* gl = &ls->dyd->gt;
    Labeldesc* gt = &gl->arr[g];
    lua_assert(eqstr(gt->name, label->name));
    if (gt->nactvar < label->nactvar) {
        TString* vname = getlocvar(fs, gt->nactvar)->varname;
        const char* msg = luaO_pushfstring(ls->L,
            "<goto %s> at line %d jumps into the scope of local '%s'",
            getstr(gt->name), gt->line, getstr(vname));
        semerror(ls, msg);
    }
    luaK_patchlist(fs, gt->pc, label->pc);
    /* remove goto from pending list */
    for (i = g; i < gl->n - 1; i++)
        gl->arr[i] = gl->arr[i + 1];
    gl->n--;
}


/*
** try to close a goto with existing labels; this solves backward jumps
*/
static int findlabel(LexState* ls, int g) {
    int i;
    BlockCnt* bl = ls->fs->bl;
    Dyndata* dyd = ls->dyd;
    Labeldesc* gt = &dyd->gt.arr[g];
    /* check labels in current block for a match */
    for (i = bl->firstlabel; i < dyd->label.n; i++) {
        Labeldesc* lb = &dyd->label.arr[i];
        if (eqstr(lb->name, gt->name)) {  /* correct label? */
            if (gt->nactvar > lb->nactvar &&
                (bl->upval || dyd->label.n > bl->firstlabel))
                luaK_patchclose(ls->fs, gt->pc, lb->nactvar);
            closegoto(ls, g, lb);  /* close it */
            return 1;
        }
    }
    return 0;  /* label not found; cannot close goto */
}


static int newlabelentry(LexState* ls, Labellist* l, TString* name,
    int line, int pc) {
    int n = l->n;
    luaM_growvector(ls->L, l->arr, n, l->size,
        Labeldesc, SHRT_MAX, "labels/gotos");
    l->arr[n].name = name;
    l->arr[n].line = line;
    l->arr[n].nactvar = ls->fs->nactvar;
    l->arr[n].pc = pc;
    l->n = n + 1;
    return n;
}


/*
** check whether new label 'lb' matches any pending gotos in current
** block; solves forward jumps
*/
static void findgotos(LexState* ls, Labeldesc* lb) {
    Labellist* gl = &ls->dyd->gt;
    int i = ls->fs->bl->firstgoto;
    while (i < gl->n) {
        if (eqstr(gl->arr[i].name, lb->name))
            closegoto(ls, i, lb);
        else
            i++;
    }
}


/*
** export pending gotos to outer level, to check them against
** outer labels; if the block being exited has upvalues, and
** the goto exits the scope of any variable (which can be the
** upvalue), close those variables being exited.
*/
static void movegotosout(FuncState* fs, BlockCnt* bl) {
    int i = bl->firstgoto;
    Labellist* gl = &fs->ls->dyd->gt;
    /* correct pending gotos to current block and try to close it
       with visible labels */
    while (i < gl->n) {
        Labeldesc* gt = &gl->arr[i];
        if (gt->nactvar > bl->nactvar) {
            if (bl->upval)
                luaK_patchclose(fs, gt->pc, bl->nactvar);
            gt->nactvar = bl->nactvar;
        }
        if (!findlabel(fs->ls, i))
            i++;  /* move to next one */
    }
}


static void enterblock(FuncState* fs, BlockCnt* bl, lu_byte isloop) {
    bl->isloop = isloop;
    bl->nactvar = fs->nactvar;
    bl->firstlabel = fs->ls->dyd->label.n;
    bl->firstgoto = fs->ls->dyd->gt.n;
    bl->upval = 0;
    bl->previous = fs->bl;
    fs->bl = bl;
    lua_assert(fs->freereg == fs->nactvar);
}


/*
** create a label named 'break' to resolve break statements
*/
static void breaklabel(LexState* ls) {
    TString* n = luaS_new(ls->L, "break");
    int l = newlabelentry(ls, &ls->dyd->label, n, 0, ls->fs->pc);
    findgotos(ls, &ls->dyd->label.arr[l]);
}

/*
** generates an error for an undefined 'goto'; choose appropriate
** message when label name is a reserved word (which can only be 'break')
*/
static l_noret undefgoto(LexState* ls, Labeldesc* gt) {
    const char* msg = isreserved(gt->name)
        ? "<%s> at line %d not inside a loop"
        : "no visible label '%s' for <goto> at line %d";
    msg = luaO_pushfstring(ls->L, msg, getstr(gt->name), gt->line);
    semerror(ls, msg);
}


static void leaveblock(FuncState* fs) {
    BlockCnt* bl = fs->bl;
    LexState* ls = fs->ls;
    if (bl->previous && bl->upval) {
        /* create a 'jump to here' to close upvalues */
        int j = luaK_jump(fs);
        luaK_patchclose(fs, j, bl->nactvar);
        luaK_patchtohere(fs, j);
    }
    if (bl->isloop)
        breaklabel(ls);  /* close pending breaks */
    fs->bl = bl->previous;
    removevars(fs, bl->nactvar);
    lua_assert(bl->nactvar == fs->nactvar);
    fs->freereg = fs->nactvar;  /* free registers */
    ls->dyd->label.n = bl->firstlabel;  /* remove local labels */
    if (bl->previous)  /* inner block? */
        movegotosout(fs, bl);  /* update pending gotos to outer block */
    else if (bl->firstgoto < ls->dyd->gt.n)  /* pending gotos in outer block? */
        undefgoto(ls, &ls->dyd->gt.arr[bl->firstgoto]);  /* error */
}


/*
** adds a new prototype into list of prototypes
*/
static Proto* addprototype(LexState* ls) {
    Proto* clp;
    lua_State* L = ls->L;
    FuncState* fs = ls->fs;
    Proto* f = fs->f;  /* prototype of current function */
    if (fs->np >= f->sizep) {
        int oldsize = f->sizep;
        luaM_growvector(L, f->p, fs->np, f->sizep, Proto*, MAXARG_Bx, "functions");
        while (oldsize < f->sizep)
            f->p[oldsize++] = NULL;
    }
    f->p[fs->np++] = clp = luaF_newproto(L);
    luaC_objbarrier(L, f, clp);
    return clp;
}


/*
** codes instruction to create new closure in parent function.
** The OP_CLOSURE instruction must use the last available register,
** so that, if it invokes the GC, the GC knows which registers
** are in use at that time.
*/
static void codeclosure(LexState* ls, expdesc* v) {
    FuncState* fs = ls->fs->prev;
    init_exp(v, VRELOCABLE, luaK_codeABx(fs, OP_CLOSURE, 0, fs->np - 1));
    luaK_exp2nextreg(fs, v);  /* fix it at the last register */
}


static void open_func(LexState* ls, FuncState* fs, BlockCnt* bl) {
    Proto* f;
    fs->prev = ls->fs;  /* linked list of funcstates */
    fs->ls = ls;
    ls->fs = fs;
    fs->pc = 0;
    fs->lasttarget = 0;
    fs->jpc = NO_JUMP;
    fs->freereg = 0;
    fs->nk = 0;
    fs->np = 0;
    fs->nups = 0;
    fs->nlocvars = 0;
    fs->nactvar = 0;
    fs->firstlocal = ls->dyd->actvar.n;
    fs->bl = NULL;
    f = fs->f;
    f->source = ls->source;
    f->maxstacksize = 2;  /* registers 0/1 are always valid */
    enterblock(fs, bl, 0);
}


static void close_func(LexState* ls) {
    lua_State* L = ls->L;
    FuncState* fs = ls->fs;
    Proto* f = fs->f;
    luaK_ret(fs, 0, 0);  /* final return */
    leaveblock(fs);
    luaM_reallocvector(L, f->code, f->sizecode, fs->pc, Instruction);
    f->sizecode = fs->pc;
    luaM_reallocvector(L, f->lineinfo, f->sizelineinfo, fs->pc, int);
    f->sizelineinfo = fs->pc;
    luaM_reallocvector(L, f->k, f->sizek, fs->nk, TValue);
    f->sizek = fs->nk;
    luaM_reallocvector(L, f->p, f->sizep, fs->np, Proto*);
    f->sizep = fs->np;
    luaM_reallocvector(L, f->locvars, f->sizelocvars, fs->nlocvars, LocVar);
    f->sizelocvars = fs->nlocvars;
    luaM_reallocvector(L, f->upvalues, f->sizeupvalues, fs->nups, Upvaldesc);
    f->sizeupvalues = fs->nups;
    lua_assert(fs->bl == NULL);
    ls->fs = fs->prev;
    luaC_checkGC(L);
}



/*============================================================*/
/* GRAMMAR RULES */
/*============================================================*/


/*
** check whether current token is in the follow set of a block.
** 'until' closes syntactical blocks, but do not close scope,
** so it is handled in separate.
*/
static int block_follow(LexState* ls, int withuntil) {
    switch (ls->t.token) {
    case TK_ELSE: case TK_ELSEIF:
    case TK_END: case TK_EOS:
        return 1;
    case TK_UNTIL: return withuntil;
    default: return 0;
    }
}


static void statlist(LexState* ls) {
    /* statlist -> { stat [';'] } */
    while (!block_follow(ls, 1)) {
        if (ls->t.token == TK_RETURN) {
            statement(ls);
            return;  /* 'return' must be last statement */
        }
        statement(ls);
    }
}


static void fieldsel(LexState* ls, expdesc* v) {
    /* fieldsel -> ['.' | ':'] NAME */
    FuncState* fs = ls->fs;
    expdesc key;
    luaK_exp2anyregup(fs, v);
    luaX_next(ls);  /* skip the dot or colon */
    checkname(ls, &key);
    luaK_indexed(fs, v, &key);
}


static void yindex(LexState* ls, expdesc* v) {
    /* index -> '[' expr ']' */
    luaX_next(ls);  /* skip the '[' */
    expr(ls, v);
    luaK_exp2val(ls->fs, v);
    checknext(ls, ']');
}


/*
** {======================================================================
** Rules for Constructors
** =======================================================================
*/

/* 表的信息 */
struct ConsControl
{
    expdesc v;  /* 存储表构造过程中最后一个表达式的信息 */
    expdesc* t;  /* 构造表相关的表达式信息, 与上一个字段的区别在于这里使用的是指针
                 ** 因为该字段是由外部传入的 */
    int nh;  /* 初始化表时, 散列部分数据的数量 */
    int na;  /* 初始化表时, 数组部分数据的数量 */
    int tostore;  /* Lua解析器中定义了一个叫 LFIELDS_PER_FLUSH 的常量, 当前的值是50
    ** 这个值的意义在于, 当前构造表时内部的数组部分的数据如果超过这个值, 就首先调用一次
    ** OP_SETLIST 函数写入寄存器中. number of array elements pending to be stored */
};

/*
** 初始化表的散列部分
** 注意会分为两种情况
** key是一个常量; 例: a = {["asfd"]=132}
** (1) 得到 key 常量在常量数组中的索引, 根据这个值调用 luaK_exp2RK 函数生成RK
** (2) 得到 value 表达式的索引, 根据这个值调用luaK_exp2RK函数生成RK
** (3) 将前两步的值以及表在寄存器中的索引, 写入OP_SETTABLE的参数中
** key是一个变量, 需要首先去获取这个变量的值. 例: a="asf"  b={[a]=132} 或 b={a=132}
**
*/
static void recfield(LexState* ls, struct ConsControl* cc)
{
    /* recfield -> (NAME | '['exp1']') = exp1 */
    FuncState* fs = ls->fs;
    int reg = ls->fs->freereg;
    expdesc key, val;
    int rkkey;
    if (ls->t.token == TK_NAME)
    {
        checklimit(fs, cc->nh, MAX_INT, "items in a constructor");
        checkname(ls, &key);
    }
    else  /* ls->t.token == '[' */
        yindex(ls, &key);
    cc->nh++;
    checknext(ls, '=');
    rkkey = luaK_exp2RK(fs, &key);
    expr(ls, &val);
    luaK_codeABC(fs, OP_SETTABLE, cc->t->u.info, rkkey, luaK_exp2RK(fs, &val));
    fs->freereg = reg;  /* free registers */
}


static void closelistfield(FuncState* fs, struct ConsControl* cc)
{
    if (cc->v.k == VVOID)
        return;  /* there is no list item */
    /* 调用luaK_exp2nextreg将前面得到的ConsControl结构体中成员v的信息存入寄存器中 */
    luaK_exp2nextreg(fs, &cc->v);
    cc->v.k = VVOID;
    /*
    ** 如果此时tostore成员的值等于LFIELDS_PER_FLUSH, 那么生成OP_SETLIST指令, 用于
    ** 将当前寄存器上的数据写入表的数组部分. 需要注意的是, 这个地方存取的数据在栈上
    ** 的位置是紧跟着OP_NEWTABLE指令中的参数A在栈上的位置, 而对OP_NEWTABLE指令格式的
    ** 解释可以知道, OP_NEWTABLE指令的参数A存放的是新创建的表在栈上的位置, 这样的话使
    ** 用一个参数既可以得到表的地址, 又可以知道待存入的数据是哪些. 之所以需要限制每次
    ** 调用OP_SETLIST指令中的数据量不超过LFIELDS_PER_FLUSH, 是因为如果不做这个限制,
    ** 会导致数组部分数据过多时, 占用过多的寄存器, 而Lua栈对寄存器数量是有限制的
    */
    if (cc->tostore == LFIELDS_PER_FLUSH)
    {
        luaK_setlist(fs, cc->t->u.info, cc->na, cc->tostore);  /* flush */
        cc->tostore = 0;  /* no more items pending */
    }
}


static void lastlistfield(FuncState* fs, struct ConsControl* cc) {
    if (cc->tostore == 0) return;
    if (hasmultret(cc->v.k)) {
        luaK_setmultret(fs, &cc->v);
        luaK_setlist(fs, cc->t->u.info, cc->na, LUA_MULTRET);
        cc->na--;  /* do not count last expression (unknown number of elements) */
    }
    else {
        if (cc->v.k != VVOID)
            luaK_exp2nextreg(fs, &cc->v);
        luaK_setlist(fs, cc->t->u.info, cc->na, cc->tostore);
    }
}

/* 表的数组部分的构造 */
static void listfield(LexState* ls, struct ConsControl* cc)
{
    /* listfield -> exp */
    /*
    **     调用expr函数解析这个表达式, 得到对应的ConsControl结构体中成员v的数据,
    ** 这个对象会暂存表构造过程中当前表达式的结果
    */
    expr(ls, &cc->v);
    /* 检查当前表中数组部分的数据数量是否超过限制了 */
    checklimit(ls->fs, cc->na, MAX_INT, "items in a constructor");
    cc->na++;
    cc->tostore++;
}


static void field(LexState* ls, struct ConsControl* cc) {
    /* field -> listfield | recfield */
    switch (ls->t.token)
    {
    case TK_NAME:
    {   /*
        **     解析到一个变量, 那么看紧跟着这个符号的是不是'=', 如果不是, 就是一
        ** 个数组方式的赋值, 否则就是散列方式的赋值
        */
        /* may be 'listfield' or 'recfield' */
        if (luaX_lookahead(ls) != '=')  /* expression? */
            listfield(ls, cc);
        else
            recfield(ls, cc);
        break;
    }
    case '[':
    {
        /* 散列部分的构造 */
        recfield(ls, cc);
        break;
    }
    default:
    {
        /* 数组部分的构造 */
        listfield(ls, cc);
        break;
    }
    }
}


static void constructor(LexState* ls, expdesc* t)
{
    /* constructor -> '{' [ field { sep field } [sep] ] '}'
      sep -> ',' | ';' */
    FuncState* fs = ls->fs;
    int line = ls->linenumber;
    /*
    **     生成一条 OP_NEWTABLE 指令, 这条指令创建的表最终会根据指令中的参数A存储在寄
    ** 存器地址, 赋值给本函数栈内的寄存器, 很显然这条指令是需要重定位的.
    */
    int pc = luaK_codeABC(fs, OP_NEWTABLE, 0, 0, 0);
    struct ConsControl cc;
    cc.na = cc.nh = cc.tostore = 0;
    cc.t = t;
    init_exp(t, VRELOCABLE, pc);
    init_exp(&cc.v, VVOID, 0);  /* no value (yet) */ /* 此时还未解析到表构造中的信息, 所以为VVOID */
    luaK_exp2nextreg(ls->fs, t);  /* fix it at stack top */ /* 解析表达式到寄存器的函数, 将寄存器地址修正为前面创建的OP_NEWTABLE 指令的参数A */
    checknext(ls, '{');
    do
    {   /* 解析 {} 中的数据 */
        lua_assert(cc.v.k == VVOID || cc.tostore > 0);
        if (ls->t.token == '}')
            break;
        /*
        ** 调用 closelistfield 函数生成上一个表达式的相关指令, 肯定会调用
        ** luaK_exp2nextreg 函数, 注意刚进入循环时, 最开始初始化 ConsControl 表达式时,
        ** 其成员变量v的表达式类型是VVOID, 因此这种情况下进入这个函数并不会有什么效果,
        */
        closelistfield(fs, &cc);
        field(ls, &cc);
    } while (testnext(ls, ',') || testnext(ls, ';'));
    check_match(ls, '}', '{', line);
    lastlistfield(fs, &cc);
    /* 将ConsControl 结构体中存放的散列和数组部分的大小, 写入前面生成的 OP_NEWTABLE 指令的B和C部分 */
    SETARG_B(fs->f->code[pc], luaO_int2fb(cc.na)); /* set initial array size */
    SETARG_C(fs->f->code[pc], luaO_int2fb(cc.nh));  /* set initial table size */
}

/* }====================================================================== */



static void parlist(LexState* ls) {
    /* parlist -> [ param { ',' param } ] */
    FuncState* fs = ls->fs;
    Proto* f = fs->f;
    int nparams = 0;
    f->is_vararg = 0;
    if (ls->t.token != ')') {  /* is 'parlist' not empty? */
        do {
            switch (ls->t.token) {
            case TK_NAME: {  /* param -> NAME */
                new_localvar(ls, str_checkname(ls));
                nparams++;
                break;
            }
            case TK_DOTS: {  /* param -> '...' */
                luaX_next(ls);
                f->is_vararg = 1;  /* declared vararg */
                break;
            }
            default: luaX_syntaxerror(ls, "<name> or '...' expected");
            }
        } while (!f->is_vararg && testnext(ls, ','));
    }
    adjustlocalvars(ls, nparams);
    f->numparams = cast_byte(fs->nactvar);
    luaK_reserveregs(fs, fs->nactvar);  /* reserve register for parameters */
}


static void body(LexState* ls, expdesc* e, int ismethod, int line) {
    /* body ->  '(' parlist ')' block END */
    FuncState new_fs;
    BlockCnt bl;
    new_fs.f = addprototype(ls);
    new_fs.f->linedefined = line;
    open_func(ls, &new_fs, &bl);  /* 初始化 FuncState 的工作 */
    checknext(ls, '(');
    if (ismethod) {
        new_localvarliteral(ls, "self");  /* create 'self' parameter */
        adjustlocalvars(ls, 1);
    }
    parlist(ls);
    checknext(ls, ')');
    statlist(ls);
    new_fs.f->lastlinedefined = ls->linenumber;
    check_match(ls, TK_END, TK_FUNCTION, line);
    codeclosure(ls, e);
    close_func(ls);
}

/*
** 对表达式列表进行解析
** 1. 调用函数 expr 解析表达式
** 2. 当解析的表达式列表中还存在其他表达式时, 即有逗号(,)分隔的式子时, 针对每个表达式
** 继续调用 expr 函数解析表达式, 将结果缓存在 expdesc 结构体中, 调用函数 luaK_exp2nextreg
** 将表达式存入当前函数的下一个可用寄存器中.
*/
static int explist(LexState* ls, expdesc* v)
{
    /* explist -> expr { ',' expr } */
    int n = 1;  /* 表达式个数, 至少有个表达式 */
    expr(ls, v);
    while (testnext(ls, ','))
    {
        luaK_exp2nextreg(ls->fs, v);  /* 根据 expdesc 生成对应的字节码 */
        expr(ls, v);
        n++;
    }
    return n;
}


static void funcargs(LexState* ls, expdesc* f, int line) {
    FuncState* fs = ls->fs;
    expdesc args;
    int base, nparams;
    switch (ls->t.token) {
    case '(': {  /* funcargs -> '(' [ explist ] ')' */
        luaX_next(ls);
        if (ls->t.token == ')')  /* arg list is empty? */
            args.k = VVOID;
        else {
            explist(ls, &args);
            luaK_setmultret(fs, &args);
        }
        check_match(ls, ')', '(', line);
        break;
    }
    case '{': {  /* funcargs -> constructor */
        constructor(ls, &args);
        break;
    }
    case TK_STRING: {  /* funcargs -> STRING */
        codestring(ls, &args, ls->t.seminfo.ts);
        luaX_next(ls);  /* must use 'seminfo' before 'next' */
        break;
    }
    default: {
        luaX_syntaxerror(ls, "function arguments expected");
    }
    }
    lua_assert(f->k == VNONRELOC);
    base = f->u.info;  /* base register for call */
    if (hasmultret(args.k))
        nparams = LUA_MULTRET;  /* open call */
    else {
        if (args.k != VVOID)
            luaK_exp2nextreg(fs, &args);  /* close last argument */
        nparams = fs->freereg - (base + 1);
    }
    init_exp(f, VCALL, luaK_codeABC(fs, OP_CALL, base, nparams + 1, 2));
    luaK_fixline(fs, line);
    fs->freereg = base + 1;  /* call remove function and arguments and leaves
                              (unless changed) one result */
}




/*
** {======================================================================
** Expression parsing
** =======================================================================
*/

/* 变量名 或者 括号包裹的表达式 */
static void primaryexp(LexState* ls, expdesc* v)
{
    /* primaryexp -> NAME | '(' expr ')' */
    switch (ls->t.token)
    {
    case '(':
    {
        int line = ls->linenumber;
        luaX_next(ls);
        expr(ls, v);
        check_match(ls, ')', '(', line);
        luaK_dischargevars(ls->fs, v);
        return;
    }
    case TK_NAME:
    {
        singlevar(ls, v);
        return;
    }
    default:
    {
        luaX_syntaxerror(ls, "unexpected symbol");
    }
    }
}


static void suffixedexp(LexState* ls, expdesc* v)
{
    /* suffixedexp ->
       primaryexp { '.' NAME | '[' exp ']' | ':' NAME funcargs | funcargs }
                    类的变量                 类的方法             函数参数    */
    FuncState* fs = ls->fs;
    int line = ls->linenumber;
    primaryexp(ls, v);  /* 解析变量名 */
    for (;;)
    {
        switch (ls->t.token)
        {
        case '.':
        {  /* fieldsel */
            fieldsel(ls, v);
            break;
        }
        case '[':
        {   /* '[' exp1 ']' */
            expdesc key;
            luaK_exp2anyregup(fs, v);
            yindex(ls, &key);
            luaK_indexed(fs, v, &key);
            break;
        }
        case ':':
        {   /* ':' NAME funcargs */
            expdesc key;
            luaX_next(ls);
            checkname(ls, &key);
            luaK_self(fs, v, &key);
            funcargs(ls, v, line);
            break;
        }
        /* print("A") | print "A" | print{["a"] = 1} */
        case '(': case TK_STRING: case '{':
        {   /* 函数参数 */
            luaK_exp2nextreg(fs, v);
            funcargs(ls, v, line);
            break;
        }
        default: return;
        }
    }
}


static void simpleexp(LexState* ls, expdesc* v)
{
    /* simpleexp -> FLT | INT | STRING | NIL | TRUE | FALSE | ... |
                    constructor | FUNCTION body | suffixedexp */
    switch (ls->t.token)
    {
    case TK_FLT: {
        init_exp(v, VKFLT, 0);
        v->u.nval = ls->t.seminfo.r;
        break;
    }
    case TK_INT:
    {
        init_exp(v, VKINT, 0);  /* int 常量 */
        v->u.ival = ls->t.seminfo.i;  /* 常量值保存在 ival 中 */
        break;
    }
    case TK_STRING: {
        codestring(ls, v, ls->t.seminfo.ts);
        break;
    }
    case TK_NIL: {
        init_exp(v, VNIL, 0);
        break;
    }
    case TK_TRUE: {
        init_exp(v, VTRUE, 0);
        break;
    }
    case TK_FALSE: {
        init_exp(v, VFALSE, 0);
        break;
    }
    case TK_DOTS: {  /* vararg */
        FuncState* fs = ls->fs;
        check_condition(ls, fs->f->is_vararg,
            "cannot use '...' outside a vararg function");
        init_exp(v, VVARARG, luaK_codeABC(fs, OP_VARARG, 0, 1, 0));
        break;
    }
    case '{': {  /* 表的构造 */
        constructor(ls, v);
        return;
    }
    case TK_FUNCTION: {
        luaX_next(ls);
        body(ls, v, 0, ls->linenumber);
        return;
    }
    default: {
        suffixedexp(ls, v);
        return;
    }
    }
    luaX_next(ls);
}


static UnOpr getunopr(int op) {
    switch (op) {
    case TK_NOT: return OPR_NOT;
    case '-': return OPR_MINUS;
    case '~': return OPR_BNOT;
    case '#': return OPR_LEN;
    default: return OPR_NOUNOPR;
    }
}


static BinOpr getbinopr(int op) {
    switch (op) {
    case '+': return OPR_ADD;
    case '-': return OPR_SUB;
    case '*': return OPR_MUL;
    case '%': return OPR_MOD;
    case '^': return OPR_POW;
    case '/': return OPR_DIV;
    case TK_IDIV: return OPR_IDIV;
    case '&': return OPR_BAND;
    case '|': return OPR_BOR;
    case '~': return OPR_BXOR;
    case TK_SHL: return OPR_SHL;
    case TK_SHR: return OPR_SHR;
    case TK_CONCAT: return OPR_CONCAT;
    case TK_NE: return OPR_NE;
    case TK_EQ: return OPR_EQ;
    case '<': return OPR_LT;
    case TK_LE: return OPR_LE;
    case '>': return OPR_GT;
    case TK_GE: return OPR_GE;
    case TK_AND: return OPR_AND;
    case TK_OR: return OPR_OR;
    default: return OPR_NOBINOPR;
    }
}

/* 左右优先级, 优先级越高对应的数字就越大 */
static const struct {
    lu_byte left;  /* left priority for each binary operator */
    lu_byte right; /* right priority */
} priority[] = {  /* ORDER OPR */
   {10, 10}, {10, 10},           /* '+' '-' */
   {11, 11}, {11, 11},           /* '*' '%' */
   {14, 13},                  /* '^' (right associative) */ /* 2^1^2 == 2^(1^2) */
   {11, 11}, {11, 11},           /* '/' '//' */
   {6, 6}, {4, 4}, {5, 5},   /* '&' '|' '~' */
   {7, 7}, {7, 7},           /* '<<' '>>' */
   {9, 8},                   /* '..' (right associative) */
   {3, 3}, {3, 3}, {3, 3},   /* ==, <, <= */
   {3, 3}, {3, 3}, {3, 3},   /* ~=, >, >= */
   {2, 2}, {1, 1}            /* and, or */
};

#define UNARY_PRIORITY  12  /* 一元运算符的优先级 */


/*
** subexpr -> (simpleexp | unop subexpr) { binop subexpr }
** where 'binop' is any binary operator with a priority higher than 'limit'
*/
/*
**     在传入函数的参数中, 其中有一个参数用于表示当前处理的表达式的优先级, 后面将根据
** 这个参数来判断在处理二元操作符时, 是先处理二元操作符左边还是右边的式子. 首次调用函
** 数时, 这个参数为0, 也就是最小的优先级.
**
**     在进入函数后, 首先判断获取到的是不是一元操作符, 如果是, 那么递归调用函数subexpr,
** 此时传入的优先级是常数 UNARY_PRIORITY; 否则调用函数 simpleexp 来处理简单的表达式
**
** 接着看读到的字符是不是二元操作符, 如果是并且同时满足这个二元操作符的优先级大于当前
** subexpr 函数的优先级, 那么递归调用函数 sumexpr 来处理二元操作符左边的式子.
*/
static BinOpr subexpr(LexState* ls, expdesc* v, int limit)
{
    BinOpr op;
    UnOpr uop;
    enterlevel(ls);
    uop = getunopr(ls->t.token);
    if (uop != OPR_NOUNOPR)
    {
        int line = ls->linenumber;
        luaX_next(ls);
        subexpr(ls, v, UNARY_PRIORITY);
        luaK_prefix(ls->fs, uop, v, line);  /* 处理一元操作符 */
    }
    else simpleexp(ls, v);
    /* expand while operators have priorities higher than 'limit' */
    op = getbinopr(ls->t.token);
    while (op != OPR_NOBINOPR && priority[op].left > limit) {
        expdesc v2;
        BinOpr nextop;
        int line = ls->linenumber;
        luaX_next(ls);
        luaK_infix(ls->fs, op, v);
        /* read sub-expression with higher priority */
        nextop = subexpr(ls, &v2, priority[op].right);
        luaK_posfix(ls->fs, op, v, &v2, line);
        op = nextop;
    }
    leavelevel(ls);
    return op;  /* return first untreated operator */
}

/* 单个表达式的解析 */
static void expr(LexState* ls, expdesc* v)
{
    subexpr(ls, v, 0);
}

/* }==================================================================== */



/*
** {======================================================================
** Rules for Statements
** =======================================================================
*/


static void block(LexState* ls) {
    /* block -> statlist */
    FuncState* fs = ls->fs;
    BlockCnt bl;
    enterblock(fs, &bl, 0);
    statlist(ls);
    leaveblock(fs);
}


/*
** structure to chain all variables in the left-hand side of an
** assignment
*/
struct LHS_assign {
    struct LHS_assign* prev;
    expdesc v;  /* variable (global, local, upvalue, or indexed) */
};


/*
** check whether, in an assignment to an upvalue/local variable, the
** upvalue/local variable is begin used in a previous assignment to a
** table. If so, save original upvalue/local value in a safe place and
** use this safe copy in the previous assignment.
*/
static void check_conflict(LexState* ls, struct LHS_assign* lh, expdesc* v) {
    FuncState* fs = ls->fs;
    int extra = fs->freereg;  /* eventual position to save local variable */
    int conflict = 0;
    for (; lh; lh = lh->prev) {  /* check all previous assignments */
        if (lh->v.k == VINDEXED) {  /* assigning to a table? */
          /* table is the upvalue/local being assigned now? */
            if (lh->v.u.ind.vt == v->k && lh->v.u.ind.t == v->u.info) {
                conflict = 1;
                lh->v.u.ind.vt = VLOCAL;
                lh->v.u.ind.t = extra;  /* previous assignment will use safe copy */
            }
            /* index is the local being assigned? (index cannot be upvalue) */
            if (v->k == VLOCAL && lh->v.u.ind.idx == v->u.info) {
                conflict = 1;
                lh->v.u.ind.idx = extra;  /* previous assignment will use safe copy */
            }
        }
    }
    if (conflict) {
        /* copy upvalue/local value to a temporary (in position 'extra') */
        OpCode op = (v->k == VLOCAL) ? OP_MOVE : OP_GETUPVAL;
        luaK_codeABC(fs, op, extra, v->u.info, 0);
        luaK_reserveregs(fs, 1);
    }
}

/* 赋值操作 */
static void assignment(LexState* ls, struct LHS_assign* lh, int nvars)
{
    expdesc e;
    check_condition(ls, vkisvar(lh->v.k), "syntax error");
    if (testnext(ls, ','))
    {   /* assignment -> ',' suffixedexp assignment */
        struct LHS_assign nv;
        nv.prev = lh;
        suffixedexp(ls, &nv.v);
        if (nv.v.k != VINDEXED)
            check_conflict(ls, lh, &nv.v);
        checklimit(ls->fs, nvars + ls->L->nCcalls, LUAI_MAXCCALLS,
            "C levels");
        assignment(ls, &nv, nvars + 1);
    }
    else {  /* assignment -> '=' explist */
        int nexps;
        checknext(ls, '=');
        nexps = explist(ls, &e);
        if (nexps != nvars)
            adjust_assign(ls, nvars, nexps, &e);
        else
        {
            luaK_setoneret(ls->fs, &e);  /* close last expression */
            luaK_storevar(ls->fs, &lh->v, &e);
            return;  /* avoid default */
        }
    }
    init_exp(&e, VNONRELOC, ls->fs->freereg - 1);  /* default assignment */
    luaK_storevar(ls->fs, &lh->v, &e);
}


static int cond(LexState* ls) {
    /* cond -> exp */
    expdesc v;
    expr(ls, &v);  /* read condition */
    if (v.k == VNIL) v.k = VFALSE;  /* 'falses' are all equal here */
    luaK_goiftrue(ls->fs, &v);
    return v.f;
}


static void gotostat(LexState* ls, int pc) {
    int line = ls->linenumber;
    TString* label;
    int g;
    if (testnext(ls, TK_GOTO))
        label = str_checkname(ls);
    else {
        luaX_next(ls);  /* skip break */
        label = luaS_new(ls->L, "break");
    }
    g = newlabelentry(ls, &ls->dyd->gt, label, line, pc);
    findlabel(ls, g);  /* close it if label already defined */
}


/* check for repeated labels on the same block */
static void checkrepeated(FuncState* fs, Labellist* ll, TString* label) {
    int i;
    for (i = fs->bl->firstlabel; i < ll->n; i++) {
        if (eqstr(label, ll->arr[i].name)) {
            const char* msg = luaO_pushfstring(fs->ls->L,
                "label '%s' already defined on line %d",
                getstr(label), ll->arr[i].line);
            semerror(fs->ls, msg);
        }
    }
}


/* skip no-op statements */
static void skipnoopstat(LexState* ls) {
    while (ls->t.token == ';' || ls->t.token == TK_DBCOLON)
        statement(ls);
}


static void labelstat(LexState* ls, TString* label, int line) {
    /* label -> '::' NAME '::' */
    FuncState* fs = ls->fs;
    Labellist* ll = &ls->dyd->label;
    int l;  /* index of new label being created */
    checkrepeated(fs, ll, label);  /* check for repeated labels */
    checknext(ls, TK_DBCOLON);  /* skip double colon */
    /* create new entry for this label */
    l = newlabelentry(ls, ll, label, line, luaK_getlabel(fs));
    skipnoopstat(ls);  /* skip other no-op statements */
    if (block_follow(ls, 0)) {  /* label is last no-op statement in the block? */
      /* assume that locals are already out of scope */
        ll->arr[l].nactvar = fs->bl->nactvar;
    }
    findgotos(ls, &ll->arr[l]);
}


static void whilestat(LexState* ls, int line) {
    /* whilestat -> WHILE cond DO block END */
    FuncState* fs = ls->fs;
    int whileinit;
    int condexit;
    BlockCnt bl;
    luaX_next(ls);  /* skip WHILE */
    whileinit = luaK_getlabel(fs);
    condexit = cond(ls);
    enterblock(fs, &bl, 1);
    checknext(ls, TK_DO);
    block(ls);
    luaK_jumpto(fs, whileinit);
    check_match(ls, TK_END, TK_WHILE, line);
    leaveblock(fs);
    luaK_patchtohere(fs, condexit);  /* false conditions finish the loop */
}


static void repeatstat(LexState* ls, int line) {
    /* repeatstat -> REPEAT block UNTIL cond */
    int condexit;
    FuncState* fs = ls->fs;
    int repeat_init = luaK_getlabel(fs);
    BlockCnt bl1, bl2;
    enterblock(fs, &bl1, 1);  /* loop block */
    enterblock(fs, &bl2, 0);  /* scope block */
    luaX_next(ls);  /* skip REPEAT */
    statlist(ls);
    check_match(ls, TK_UNTIL, TK_REPEAT, line);
    condexit = cond(ls);  /* read condition (inside scope block) */
    if (bl2.upval)  /* upvalues? */
        luaK_patchclose(fs, condexit, bl2.nactvar);
    leaveblock(fs);  /* finish scope */
    luaK_patchlist(fs, condexit, repeat_init);  /* close the loop */
    leaveblock(fs);  /* finish loop */
}


static int exp1(LexState* ls) {
    expdesc e;
    int reg;
    expr(ls, &e);
    luaK_exp2nextreg(ls->fs, &e);
    lua_assert(e.k == VNONRELOC);
    reg = e.u.info;
    return reg;
}


static void forbody(LexState* ls, int base, int line, int nvars, int isnum) {
    /* forbody -> DO block */
    BlockCnt bl;
    FuncState* fs = ls->fs;
    int prep, endfor;
    adjustlocalvars(ls, 3);  /* control variables */
    checknext(ls, TK_DO);
    prep = isnum ? luaK_codeAsBx(fs, OP_FORPREP, base, NO_JUMP) : luaK_jump(fs);
    enterblock(fs, &bl, 0);  /* scope for declared variables */
    adjustlocalvars(ls, nvars);
    luaK_reserveregs(fs, nvars);
    block(ls);
    leaveblock(fs);  /* end of scope for declared variables */
    luaK_patchtohere(fs, prep);
    if (isnum)  /* numeric for? */
        endfor = luaK_codeAsBx(fs, OP_FORLOOP, base, NO_JUMP);
    else {  /* generic for */
        luaK_codeABC(fs, OP_TFORCALL, base, 0, nvars);
        luaK_fixline(fs, line);
        endfor = luaK_codeAsBx(fs, OP_TFORLOOP, base + 2, NO_JUMP);
    }
    luaK_patchlist(fs, endfor, prep + 1);
    luaK_fixline(fs, line);
}


static void fornum(LexState* ls, TString* varname, int line) {
    /* fornum -> NAME = exp1,exp1[,exp1] forbody */
    FuncState* fs = ls->fs;
    int base = fs->freereg;
    new_localvarliteral(ls, "(for index)");
    new_localvarliteral(ls, "(for limit)");
    new_localvarliteral(ls, "(for step)");
    new_localvar(ls, varname);
    checknext(ls, '=');
    exp1(ls);  /* initial value */
    checknext(ls, ',');
    exp1(ls);  /* limit */
    if (testnext(ls, ','))
        exp1(ls);  /* optional step */
    else {  /* default step = 1 */
        luaK_codek(fs, fs->freereg, luaK_intK(fs, 1));
        luaK_reserveregs(fs, 1);
    }
    forbody(ls, base, line, 1, 1);
}


static void forlist(LexState* ls, TString* indexname) {
    /* forlist -> NAME {,NAME} IN explist forbody */
    FuncState* fs = ls->fs;
    expdesc e;
    int nvars = 4;  /* gen, state, control, plus at least one declared var */
    int line;
    int base = fs->freereg;
    /* create control variables */
    new_localvarliteral(ls, "(for generator)");
    new_localvarliteral(ls, "(for state)");
    new_localvarliteral(ls, "(for control)");
    /* create declared variables */
    new_localvar(ls, indexname);
    while (testnext(ls, ',')) {
        new_localvar(ls, str_checkname(ls));
        nvars++;
    }
    checknext(ls, TK_IN);
    line = ls->linenumber;
    adjust_assign(ls, 3, explist(ls, &e), &e);
    luaK_checkstack(fs, 3);  /* extra space to call generator */
    forbody(ls, base, line, nvars - 3, 0);
}


static void forstat(LexState* ls, int line) {
    /* forstat -> FOR (fornum | forlist) END */
    FuncState* fs = ls->fs;
    TString* varname;
    BlockCnt bl;
    enterblock(fs, &bl, 1);  /* scope for loop and control variables */
    luaX_next(ls);  /* skip 'for' */
    varname = str_checkname(ls);  /* first variable name */
    switch (ls->t.token) {
    case '=': fornum(ls, varname, line); break;
    case ',': case TK_IN: forlist(ls, varname); break;
    default: luaX_syntaxerror(ls, "'=' or 'in' expected");
    }
    check_match(ls, TK_END, TK_FOR, line);
    leaveblock(fs);  /* loop scope ('break' jumps to this point) */
}


static void test_then_block(LexState* ls, int* escapelist)
{
    /* test_then_block -> [IF | ELSEIF] cond THEN block */
    BlockCnt bl;
    FuncState* fs = ls->fs;
    expdesc v;
    int jf;  /* instruction to skip 'then' code (if condition is false) */
    luaX_next(ls);  /* skip IF or ELSEIF */
    expr(ls, &v);  /* read condition */
    checknext(ls, TK_THEN);
    if (ls->t.token == TK_GOTO || ls->t.token == TK_BREAK) {
        luaK_goiffalse(ls->fs, &v);  /* will jump to label if condition is true */
        enterblock(fs, &bl, 0);  /* must enter block before 'goto' */
        gotostat(ls, v.t);  /* handle goto/break */
        while (testnext(ls, ';')) {}  /* skip colons */
        if (block_follow(ls, 0)) {  /* 'goto' is the entire block? */
            leaveblock(fs);
            return;  /* and that is it */
        }
        else  /* must skip over 'then' part if condition is false */
            jf = luaK_jump(fs);
    }
    else {  /* regular case (not goto/break) */
        luaK_goiftrue(ls->fs, &v);  /* skip over block if condition is false */
        enterblock(fs, &bl, 0);
        jf = v.f;
    }
    statlist(ls);  /* 'then' part */
    leaveblock(fs);
    if (ls->t.token == TK_ELSE ||
        ls->t.token == TK_ELSEIF)  /* followed by 'else'/'elseif'? */
        luaK_concat(fs, escapelist, luaK_jump(fs));  /* must jump over it */
    luaK_patchtohere(fs, jf);
}

/* IF 语句 */
static void ifstat(LexState* ls, int line)
{
    /* ifstat -> IF cond THEN block {ELSEIF cond THEN block} [ELSE block] END */
    FuncState* fs = ls->fs;
    int escapelist = NO_JUMP;  /* exit list for finished parts */
    test_then_block(ls, &escapelist);  /* IF cond THEN block */
    while (ls->t.token == TK_ELSEIF)
        test_then_block(ls, &escapelist);  /* ELSEIF cond THEN block */
    if (testnext(ls, TK_ELSE))
        block(ls);  /* 'else' part */
    check_match(ls, TK_END, TK_IF, line);
    luaK_patchtohere(fs, escapelist);  /* patch escape list to 'if' end */
}


static void localfunc(LexState* ls) {
    expdesc b;
    FuncState* fs = ls->fs;
    new_localvar(ls, str_checkname(ls));  /* new local variable */
    adjustlocalvars(ls, 1);  /* enter its scope */
    body(ls, &b, 0, ls->linenumber);  /* function created in next register */
    /* debug information will only see the variable after this point! */
    getlocvar(fs, b.u.info)->startpc = fs->pc;
}

/*
**     首先读取'='号左边的所有变量, 并在 Proto 结构体中创建相应的局部变量信息; 而变量
** 在Lua函数栈中的存储位置存放在freereg变量中, 它会根据当前函数栈中变量的数量进行调整.
*/
static void localstat(LexState* ls)
{
    /* stat -> LOCAL NAME {',' NAME} ['=' explist] */
    /* 局部变量的定义 */
    int nvars = 0;  /* 变量数(以','分隔) */
    int nexps;  /* 表达式数(以','分隔) */
    expdesc e;
    do
    {   /* 将等号'='左边的所有以','分隔的变量都生成一个相应的局部变量 */
        /* 注: 并没有判断局部变量是否重名 */
        new_localvar(ls, str_checkname(ls));  /* 使用 LocVar 存储局部变量 */
        nvars++;
    } while (testnext(ls, ','));
    /* 对表达式的解析 */
    if (testnext(ls, '='))  /* 判断是否有初始值 */
    {
        nexps = explist(ls, &e);
    }
    else
    {
        e.k = VVOID;
        nexps = 0;
    }
    /*
    **     根据等号两边变量和表达式的数量来调整赋值, 当变量数量多于等号右边的表达式数
    ** 量时, 会将多余的变量置为 NIL.
    */
    adjust_assign(ls, nvars, nexps, &e);
    /*
    **     根据变量的数量调整 FuncState 结构体中记录局部变量数量的nactvar对象, 并记录
    ** 这些局部变量的 startpc 值.
    */
    adjustlocalvars(ls, nvars);
}


static int funcname(LexState* ls, expdesc* v) {
    /* funcname -> NAME {fieldsel} [':' NAME] */
    int ismethod = 0;
    singlevar(ls, v);
    while (ls->t.token == '.')
        fieldsel(ls, v);
    if (ls->t.token == ':') {
        ismethod = 1;
        fieldsel(ls, v);
    }
    return ismethod;
}


static void funcstat(LexState* ls, int line)
{
    /* funcstat -> FUNCTION funcname body */
    int ismethod;
    expdesc v, b;  /* 表达式 v 用来存放函数名信息, 表达式 b 用于保存函数体信息 */
    luaX_next(ls);  /* skip FUNCTION */
    ismethod = funcname(ls, &v);  /* 解析函数名, 并将结果保存在变量 v 中 */
    body(ls, &b, ismethod, line);  /* 解析函数体, 并将返回的信息存放在 b 中 */
    luaK_storevar(ls->fs, &v, &b);  /* 将解析出的 body 信息和函数名 v 对应上 */
    luaK_fixline(ls->fs, line);  /* definition "happens" in the first line */
}

/* 函数调用 或 赋值语句 */
static void exprstat(LexState* ls)
{
    /* stat -> func | assignment */
    FuncState* fs = ls->fs;
    struct LHS_assign v;  /* 为了处理多变量赋值的情况, 如a,b,c=..., v.v类型为
                           * expdesc描述了等号左边的变量 */
    suffixedexp(ls, &v.v); /* 解析第一个名称 */
    if (ls->t.token == '=' || ls->t.token == ',')
    {   /* 赋值语句 a = 1 or a, b, c = 1, 2 */
        v.prev = NULL;
        assignment(ls, &v, 1);  /* 解析之后的语法 */
    }
    else
    {  /* stat -> func */
        check_condition(ls, v.v.k == VCALL, "syntax error");  /* 确保为函数调用 */
        SETARG_C(getinstruction(fs, &v.v), 1);  /* call statement uses no results */
    }
}


static void retstat(LexState* ls) {
    /* stat -> RETURN [explist] [';'] */
    FuncState* fs = ls->fs;
    expdesc e;
    int first, nret;  /* registers with returned values */
    if (block_follow(ls, 1) || ls->t.token == ';')
        first = nret = 0;  /* return no values */
    else {
        nret = explist(ls, &e);  /* optional return values */
        if (hasmultret(e.k)) {
            luaK_setmultret(fs, &e);
            if (e.k == VCALL && nret == 1) {  /* tail call? */
                SET_OPCODE(getinstruction(fs, &e), OP_TAILCALL);
                lua_assert(GETARG_A(getinstruction(fs, &e)) == fs->nactvar);
            }
            first = fs->nactvar;
            nret = LUA_MULTRET;  /* return all values */
        }
        else {
            if (nret == 1)  /* only one single value? */
                first = luaK_exp2anyreg(fs, &e);
            else {
                luaK_exp2nextreg(fs, &e);  /* values must go to the stack */
                first = fs->nactvar;  /* return all active values */
                lua_assert(nret == fs->freereg - first);
            }
        }
    }
    luaK_ret(fs, first, nret);
    testnext(ls, ';');  /* skip optional semicolon */
}


static void statement(LexState* ls) {
    int line = ls->linenumber;  /* may be needed for error messages */
    enterlevel(ls);
    switch (ls->t.token) {
    case ';': {  /* stat -> ';' (empty statement) */
        luaX_next(ls);  /* skip ';' */
        break;
    }
    case TK_IF: {  /* stat -> ifstat */
        ifstat(ls, line);
        break;
    }
    case TK_WHILE: {  /* stat -> whilestat */
        whilestat(ls, line);
        break;
    }
    case TK_DO: {  /* stat -> DO block END */
        luaX_next(ls);  /* skip DO */
        block(ls);
        check_match(ls, TK_END, TK_DO, line);
        break;
    }
    case TK_FOR: {  /* stat -> forstat */
        forstat(ls, line);
        break;
    }
    case TK_REPEAT: {  /* stat -> repeatstat */
        repeatstat(ls, line);
        break;
    }
    case TK_FUNCTION: {  /* stat -> funcstat */
        funcstat(ls, line);
        break;
    }
    case TK_LOCAL:  /* 定义局部变量 */
    {   /* stat -> localstat */
        luaX_next(ls);  /* skip LOCAL */
        if (testnext(ls, TK_FUNCTION))  /* 局部函数? */
            localfunc(ls);
        else
            localstat(ls);
        break;
    }
    case TK_DBCOLON: {  /* stat -> label */
        luaX_next(ls);  /* skip double colon */
        labelstat(ls, str_checkname(ls), line);
        break;
    }
    case TK_RETURN: {  /* stat -> retstat */
        luaX_next(ls);  /* skip RETURN */
        retstat(ls);
        break;
    }
    case TK_BREAK:   /* stat -> breakstat */
    case TK_GOTO: {  /* stat -> 'goto' NAME */
        gotostat(ls, luaK_jump(ls->fs));
        break;
    }
    default: {  /* stat -> func | assignment */
        exprstat(ls);  /* 函数调用 或 赋值语句 */
        break;
    }
    }
    lua_assert(ls->fs->f->maxstacksize >= ls->fs->freereg &&
        ls->fs->freereg >= ls->fs->nactvar);
    ls->fs->freereg = ls->fs->nactvar;  /* free registers */
    leavelevel(ls);
}

/* }====================================================================== */


/*
** compiles the main function, which is a regular vararg function with an
** upvalue named LUA_ENV
*/
static void mainfunc(LexState* ls, FuncState* fs)
{
    BlockCnt bl;
    expdesc v;
    open_func(ls, fs, &bl);
    fs->f->is_vararg = 1;  /* main function is always declared vararg */
    init_exp(&v, VLOCAL, 0);  /* create and... */
    newupvalue(fs, ls->envn, &v);  /* ...set environment upvalue */
    luaX_next(ls);  /* read first token */
    statlist(ls);  /* parse main body */
    check(ls, TK_EOS);
    close_func(ls);
}


LClosure* luaY_parser(lua_State* L, ZIO* z, Mbuffer* buff,
    Dyndata* dyd, const char* name, int firstchar)
{
    LexState lexstate;
    FuncState funcstate;
    LClosure* cl = luaF_newLclosure(L, 1);  /* create main closure */
    setclLvalue(L, L->top, cl);  /* anchor it (to avoid being collected) */
    luaD_inctop(L);
    lexstate.h = luaH_new(L);  /* create table for scanner */
    sethvalue(L, L->top, lexstate.h);  /* anchor it */
    luaD_inctop(L);
    funcstate.f = cl->p = luaF_newproto(L);
    funcstate.f->source = luaS_new(L, name);  /* create and anchor TString */
    lua_assert(iswhite(funcstate.f));  /* do not need barrier here */
    lexstate.buff = buff;
    lexstate.dyd = dyd;
    dyd->actvar.n = dyd->gt.n = dyd->label.n = 0;
    luaX_setinput(L, &lexstate, z, funcstate.f->source, firstchar);
    mainfunc(&lexstate, &funcstate);
    lua_assert(!funcstate.prev && funcstate.nups == 1 && !lexstate.fs);
    /* all scopes should be correctly finished */
    lua_assert(dyd->actvar.n == 0 && dyd->gt.n == 0 && dyd->label.n == 0);
    L->top--;  /* remove scanner's table */
    return cl;  /* closure is on the stack, too */
}

