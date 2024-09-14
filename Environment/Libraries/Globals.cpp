//
// Created by Dottik on 18/8/2024.
//

#include "Globals.hpp"

#include <HttpStatus.hpp>
#include <iostream>
#include <lz4.h>

#include "ClosureManager.hpp"
#include "Communication/Communication.hpp"
#include "Luau/CodeGen/include/Luau/CodeGen.h"
#include "Luau/Compiler.h"
#include "RobloxManager.hpp"
#include "Scheduler.hpp"
#include "Security.hpp"
#include "cpr/api.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lstring.h"

namespace RbxStu {
    int isluau(lua_State *L) {
        lua_pushboolean(L, true);
        return 1;
    }

    int makeuncollectable(lua_State *L) {
        luaL_checkany(L, 1);
        luaC_threadbarrier(L);
        s_mRefsMap[luaA_toobject(L, 1)->value.p] = lua_ref(L, 1);
        return 0;
    }

    int makecollectable(lua_State *L) {
        luaL_checkany(L, 1);
        const auto *const obj = luaA_toobject(L, 1);

        if (obj->tt == lua_Type::LUA_TFUNCTION && obj->value.gc->cl.isC) { // This could have dire consequences...
            luaL_error(L, "C closures cannot be made collectable");
        } else if (obj->tt == lua_Type::LUA_TFUNCTION &&
                   !obj->value.gc->cl.isC) { // This could have dire consequences...
            // We must check if this closure is part of a Wrapped C Closure, if so, we cannot make it collectable, else
            // on call, the newcclosure will crash us.
            if (ClosureManager::GetSingleton()->IsWrapped(static_cast<const Closure *>(obj->value.p)))
                luaL_error(L, "luau closures wrapped with a new C closure cannot be made collectable");
        } else if (obj->value.p == L->global->registry.value.p) {
            luaL_error(L, "The luau registry cannot be made collectable");
        }

        luaC_threadbarrier(L);

        // If an object was referenced in the Lua Registry, we must find it, and unref it to allow it to be collected
        // Which steps aside from making it white (refer to luaC_init).
        if (s_mRefsMap.contains(luaA_toobject(L, 1)->value.p)) {
            lua_unref(L, s_mRefsMap.at(luaA_toobject(L, 1)->value.p));
            s_mRefsMap.erase(luaA_toobject(L, 1)->value.p);
        }

        obj->value.gc->gch.marked = luaC_white(L->global);
        return 0;
    }

    int printaddress(lua_State *L) {
        luaL_checkany(L, 1);
        auto p = lua_topointer(L, 1);
        Logger::GetSingleton()->PrintInformation(RbxStu::Anonymous, std::format("Given Object: {}", p));
        return 0;
    }

    int gethwid(lua_State *L) {
        lua_pushstring(L, Communication::GetSingleton()->GetHardwareId().c_str());
        return 1;
    }

    int isnativecode(lua_State *L) {
        luaL_checktype(L, 1, lua_Type::LUA_TFUNCTION);

        const auto cl = lua_toclosure(L, 1);

        lua_pushboolean(L, cl->isC || cl->l.p->execdata != nullptr);
        return 1;
    }

    int compiletonative(lua_State *L) {
        luaL_checktype(L, 1, lua_Type::LUA_TFUNCTION);

        const Luau::CodeGen::CompilationOptions opts{Luau::CodeGen::CodeGenFlags::CodeGen_ColdFunctions};
        Logger::GetSingleton()->PrintInformation(RbxStu::Anonymous, "Compiling function into native code...");
        Luau::CodeGen::compile(L, -1, opts);

        return 0;
    }

    void deoptimize_all_protos(Proto *p) {
        if (p == nullptr)
            return;

        for (int i = 0; i < p->sizep; i++) {
            auto currentProto = (p + i);
            currentProto->flags = 0;
            currentProto->exectarget = 0;
            free(currentProto->execdata);
            currentProto->execdata = nullptr;
        }
    }

    int deoptimizefromnative(lua_State *L) {
        luaL_checktype(L, 1, lua_Type::LUA_TFUNCTION);

        const auto cl = lua_toclosure(L, 1);

        if (cl->isC)
            luaL_argerror(L, 1, "Lua closure expected");

        auto currentCi = L->ci;
        while (L->base_ci < currentCi) {
            if (currentCi->func->value.p == cl)
                luaL_argerror(L, 1, "The function to deoptimize from native cannot be on the callstack");

            currentCi--;
        }

        deoptimize_all_protos(cl->l.p);

        return 0;
    }
} // namespace RbxStu


std::string Globals::GetLibraryName() { return "rbxstu"; }
luaL_Reg *Globals::GetLibraryFunctions() {
    // WARNING: you MUST add nullptr at the end of luaL_Reg declarations, else, Luau will choke.
    const auto reg = new luaL_Reg[]{{"isluau", RbxStu::isluau},
                                    // {"decompile", RbxStu::decompile}, // Stripped for the reasons of security.
                                    {"makeuncollectable", RbxStu::makeuncollectable},
                                    {"makecollectable", RbxStu::makecollectable},
                                    {"printaddress", RbxStu::printaddress},
                                    {"gethwid", RbxStu::gethwid},
                                    {"compiletonative", RbxStu::compiletonative},
                                    {"isnativecode", RbxStu::isnativecode},
                                    {"deoptimizefromnative", RbxStu::deoptimizefromnative},
                                    {nullptr, nullptr}};

    return reg;
}
