#include <Environment/Libraries/WebSocket.hpp>
#include <Scheduler.hpp>
#include <ThemidaSDK.h>
#include <iostream>
#include <shared_mutex>

#include "ClosureManager.hpp"
#include "Communication/Communication.hpp"
#include "Environment/EnvironmentManager.hpp"
#include "Environment/Libraries/Globals.hpp"
#include "Luau/CodeGen/include/Luau/CodeGen.h"
#include "Luau/Compiler.h"
#include "Luau/Compiler/src/Builtins.h"
#include "LuauManager.hpp"
#include "ModLoader/ModManager.hpp"
#include "RobloxManager.hpp"
#include "Security.hpp"
#include "lstate.h"
#include "lualib.h"

std::mutex SchedulerJob::__scheduler__job__access__mutex;

std::shared_ptr<Scheduler> Scheduler::pInstance;

std::shared_ptr<Scheduler> Scheduler::GetSingleton() {
    if (Scheduler::pInstance == nullptr)
        Scheduler::pInstance = std::make_shared<Scheduler>();

    return Scheduler::pInstance;
}

void Scheduler::ScheduleJob(SchedulerJob job) { this->m_qSchedulerJobs.emplace(job); }

SchedulerJob Scheduler::GetSchedulerJob(bool pop) {
    if (this->m_qSchedulerJobs.empty())
        return SchedulerJob("");
    auto job = this->m_qSchedulerJobs.front();
    if (pop)
        this->m_qSchedulerJobs.pop();
    return job;
}

bool Scheduler::ExecuteSchedulerJob(lua_State *runOn, SchedulerJob *job) {
    VM_START;
    STR_ENCRYPT_START;
    const auto logger = Logger::GetSingleton();
    const auto robloxManager = RobloxManager::GetSingleton();
    const auto security = Security::GetSingleton();
    if (job->bIsLuaCode) {
        if (job->luaJob.szluaCode.empty())
            return true;

        logger->PrintInformation(RbxStu::Scheduler, "Compiling Bytecode...");

        auto opts = Luau::CompileOptions{};
        opts.debugLevel = 2;
        opts.optimizationLevel =
                1; // O2 enables inlining, this breaks hookfunction in some cases. and thus, it should be 1.
        const char *mutableGlobals[] = {"_G", "_ENV", "shared", nullptr};
        opts.mutableGlobals = mutableGlobals;
        const auto bytecode =
                Luau::compile(std::string("(function() script = Instance.new('LocalScript');getgenv()['string'] "
                                          "= getrawmetatable('').__index; end)(); task.spawn(function()")
                                      .append(job->luaJob.szluaCode)
                                      .append("; end);"),
                              opts);

        logger->PrintInformation(RbxStu::Scheduler, "Compiled Bytecode!");

        // WARNING: This code will have to be run ONLY if you decide to use luaE_newthread, as lua_newthread will
        // execute the callback, which will trigger it and initialize the RobloxExtraSpace correctly.
        // runOn->global->cb.userthread(runOn, L);
        auto L = lua_newthread(runOn);
        lua_pop(runOn, 1);

        security->SetThreadSecurity(L, 8);

        if (luau_load(L, std::format("RbxStuV2__{}", job->luaJob.szOperationIdentifier).c_str(), bytecode.c_str(),
                      bytecode.size(), 0) != LUA_OK) {
            const char *err = lua_tostring(L, -1);
            logger->PrintError(RbxStu::Scheduler, err);
            ExecutionStatus status{};
            status.Status = ScheduleLuauResponsePacketFlags::Failure;
            status.szOperationIdentifier = job->luaJob.szOperationIdentifier;
            status.szErrorMessage = err;
            Communication::GetSingleton()->ReportExecutionStatus(status);
            lua_xmove(L, runOn, 1); // Move error to rL.
            return true;
        }
        logger->PrintWarning(RbxStu::Scheduler,
                             std::format("lua_State *nL = {:#x};", reinterpret_cast<std::uintptr_t>(L)));

        auto *pClosure = const_cast<Closure *>(static_cast<const Closure *>(lua_topointer(L, -1)));

        security->SetLuaClosureSecurity(pClosure, 8);

        if (Communication::GetSingleton()->IsCodeGenerationEnabled()) {
            const Luau::CodeGen::CompilationOptions opts{Luau::CodeGen::CodeGenFlags::CodeGen_ColdFunctions};
            logger->PrintInformation(RbxStu::Scheduler,
                                     "Native Code Generation is enabled! Compiling Luau Bytecode -> Native");
            Luau::CodeGen::compile(L, -1, opts);
        }

        if (robloxManager->GetRobloxTaskDefer().has_value()) {
            const auto defer = robloxManager->GetRobloxTaskDefer().value();
            logger->PrintInformation(RbxStu::Scheduler, "Scheduling via RBX::ScriptContext::task_defer...");
            defer(L);
        } else if (robloxManager->GetRobloxTaskSpawn().has_value()) {
            const auto spawn = robloxManager->GetRobloxTaskSpawn().value();
            logger->PrintInformation(RbxStu::Scheduler, "Scheduling via RBX::ScriptContext::task_spawn...");
            spawn(L);
        } else {
            logger->PrintError(RbxStu::Scheduler,
                               "Execution attempt failed. There is no function that can run the code through Roblox's "
                               "scheduler! Reason: task.defer and task.spawn were not found on the sigging step.");

            throw std::exception("Cannot run Scheduler job!");
        }

        ExecutionStatus status{};
        status.Status = ScheduleLuauResponsePacketFlags::Success;
        status.szOperationIdentifier = job->luaJob.szOperationIdentifier;
        status.szErrorMessage = "no error";
        Communication::GetSingleton()->ReportExecutionStatus(status);
        return true;
    } else if (job->bIsYieldingJob) {
        if (job->IsJobCompleted()) {
            // If the job is completed, we want to call RBX::ScriptContext::resume using it!
            // else we want to enqueue it again to check it on the next cycle of the scheduler, as the yielding will
            // else never end!

            if (const auto callback = job->GetCallback(); callback.has_value()) {
                const auto nargs = callback.value()(runOn);
                if (nargs == -1) { // Script errored!
                    logger->PrintWarning(RbxStu::Scheduler, "Resuming with error!");
                    lua_xmove(runOn, job->yieldJob.threadRef.thread, lua_gettop(runOn));
                    const auto szError = lua_tostring(job->yieldJob.threadRef.thread, -1);
                    robloxManager->ResumeScript(&job->yieldJob.threadRef, 1, true, szError);
                } else {
                    logger->PrintWarning(RbxStu::Scheduler, "Starting resumption!");
                    lua_xmove(runOn, job->yieldJob.threadRef.thread, lua_gettop(runOn));
                    robloxManager->ResumeScript(&job->yieldJob.threadRef, nargs, false, nullptr);
                }

                job->FreeResources();
            } else {
                logger->PrintError(RbxStu::Scheduler,
                                   "Callback has no value despite the job being marked as completed!");
            }

            return true;
        } else {
            // Due to the nature of this architecture, when we have only one job left, we will just start calling it
            // again and again Which is truly terrible for performance, and I should refactor to separate the execution
            // from the resumption, but whats done its done...

            // if (this->m_qSchedulerJobs.size() == 1)
            //     _mm_pause();
            return false;
        }
    }

    logger->PrintError(RbxStu::Scheduler, "Cannot find a valid job to step into; not even a stub one!");
    const auto invalidJob = "Valid job not found!";
    STR_ENCRYPT_END;
    VM_END;
    throw std::exception(invalidJob);
};

std::shared_mutex __scheduler_init;

std::optional<lua_State *> Scheduler::GetGlobalExecutorState() const {
    std::lock_guard g{__scheduler_init};
    return this->m_lsInitialisedWith;
}

std::optional<lua_State *> Scheduler::GetGlobalRobloxState() const {
    std::lock_guard g{__scheduler_init};
    return this->m_lsRoblox;
}

std::shared_mutex __scheduler_lock;

void Scheduler::StepScheduler(lua_State *runner) {
    STR_ENCRYPT_START;
    std::scoped_lock lg{__scheduler_lock};
    // Here we will check if the DataModel obtained is correct, as in, our data model is successful!
    const auto robloxManager = RobloxManager::GetSingleton();
    const auto logger = Logger::GetSingleton();
    if (const auto dataModel = robloxManager->GetCurrentDataModel(this->GetExecutionDataModel());
        !dataModel.has_value() || this->m_pClientDataModel.value() != dataModel.value()) {
        logger->PrintWarning(RbxStu::Scheduler, "The task scheduler's internal state is out of date! Reason: DataModel "
                                                "pointer is invalid! Executing reinitialization sub-routine!");
        this->ResetScheduler(Utilities::IsPointerValid(this->m_pClientDataModel.value()) &&
                                             robloxManager->GetDataModelType(this->m_pClientDataModel.value()) !=
                                                     robloxManager->GetDataModelType(dataModel.value())
                                     ? SchedulerResetReason::ExecutionDataModelChanged
                                     : SchedulerResetReason::DataModelLost);
        return;
    }
    // if (runner != this->m_lsInitialisedWith.value()) {
    //     logger->PrintWarning(
    //             RbxStu::Scheduler,
    //             "The task scheduler's RbxStu state is different than the provided one to execute operations on. This
    //             " "may result in wrong behaviour inside of Luau and could potentially leak the elevated RbxStu "
    //             "environment into segments which are not supposed to have such elevated access! Reason: gt and L are
    //             different!");
    // }
    auto job = this->GetSchedulerJob(false);
    this->GetSchedulerJob(this->ExecuteSchedulerJob(runner, &job));

    if (lua_type(runner, -1) == lua_Type::LUA_TSTRING) {
        logger->PrintWarning(RbxStu::Scheduler, "Dispatching compilation error into Roblox Studio!");
        lua_error(runner);
    }
    STR_ENCRYPT_END;
}

void Scheduler::SetExecutionDataModel(RBX::DataModelType dataModel) {
    Communication::GetSingleton()->SetExecutionDataModel(dataModel);
    Logger::GetSingleton()->PrintWarning(RbxStu::Scheduler, "Execution DataModel changed. Issuing Scheduler Reset!");
    this->ResetScheduler(SchedulerResetReason::ExecutionDataModelChanged);
}

RBX::DataModelType Scheduler::GetExecutionDataModel() { return Communication::GetSingleton()->GetExecutionDataModel(); }

void Scheduler::InitializeWith(lua_State *L, lua_State *rL, RBX::DataModel *dataModel) {
    std::lock_guard g{__scheduler_init};
    const auto logger = Logger::GetSingleton();
    const auto robloxManager = RobloxManager::GetSingleton();

    if (const auto luauManager = LuauManager::GetSingleton();
        !robloxManager->IsInitialized() || !luauManager->IsInitialized()) {
        logger->PrintWarning(RbxStu::Scheduler, "LuauManager/RobloxManager is not initialized yet...");
        return;
    }
    this->m_pClientDataModel = dataModel;
    this->m_lsRoblox = rL;
    this->m_lsInitialisedWith = L;

    logger->PrintInformation(RbxStu::Scheduler,
                             std::format("Task Scheduler initialized!\nInternal State: \n\t- m_pClientDataModel: "
                                         "{}\n\t- m_lsRoblox: {}\n\t- m_lsInitialisedWith: {}",
                                         reinterpret_cast<void *>(this->m_pClientDataModel.value()),
                                         reinterpret_cast<void *>(this->m_lsRoblox.value()),
                                         reinterpret_cast<void *>(this->m_lsInitialisedWith.value())));

    const auto security = Security::GetSingleton();

    logger->PrintInformation(RbxStu::Scheduler, "Initializing Environment for the executor thread!");

    luaL_sandboxthread(L);

    const auto envManager = EnvironmentManager::GetSingleton();
    envManager->PushEnvironment(L);

    logger->PrintInformation(RbxStu::Scheduler, "Elevating!");

    security->SetThreadSecurity(rL, 8);
    security->SetThreadSecurity(L, 8);

    logger->PrintInformation(RbxStu::Scheduler, "Initializing Heartbeat signal...");

    lua_pushcclosure(
            L,
            [](lua_State *L) -> std::int32_t {
                const auto scheduler = Scheduler::GetSingleton();
                if (scheduler->IsInitialized())
                    scheduler->StepScheduler(L);
                return 0;
            },
            nullptr, 0);

    lua_setglobal(L, std::format("scheduler_", reinterpret_cast<std::uintptr_t>(L)).c_str());

    auto opts = Luau::CompileOptions{};
    opts.debugLevel = 2;
    opts.optimizationLevel = 0;
    const auto bytecode =
            Luau::compile(std::format("print\"hi\"; return game:GetService(\"RunService\").Heartbeat:Connect({})",
                                      std::format("scheduler_", reinterpret_cast<std::uintptr_t>(L))),
                          opts);


    if (luau_load(L, "SchedulerHookInit", bytecode.c_str(), bytecode.size(), 0) != LUA_OK) {
        const char *err = lua_tostring(L, -1);
        logger->PrintError(RbxStu::Scheduler, err);
        lua_pop(L, 1);
        return;
    }

    security->SetLuaClosureSecurity(lua_toclosure(L, -1), 8);

    Luau::CodeGen::CompilationOptions nativeOptions{Luau::CodeGen::CodeGenFlags::CodeGen_ColdFunctions};
    Luau::CodeGen::compile(L, -1, nativeOptions);

    // if (robloxManager->GetRobloxTaskDefer().has_value()) {
    //     const auto defer = robloxManager->GetRobloxTaskDefer().value();
    //     defer(L);
    // } else if (robloxManager->GetRobloxTaskSpawn().has_value()) {
    //     const auto spawn = robloxManager->GetRobloxTaskSpawn().value();
    //     spawn(L);
    // } else {
    //     logger->PrintError(RbxStu::Scheduler,
    //                        "Execution attempt failed. There is no function that can run the code through Roblox's "
    //                        "scheduler! Reason: task.defer and task.spawn were not found on the sigging step.");
    //     throw std::exception("Cannot run Scheduler job!");
    // }
    lua_call(L, 0, 1);

    this->m_lRefToStepConnection = lua_ref(L, -1);
    lua_pop(L, 1);

    logger->PrintInformation(RbxStu::Scheduler, "Initialized!");

    logger->PrintInformation(RbxStu::Scheduler, "Invoking OnSchedulerInitialized mod subroutines...");
    ModManager::GetSingleton()->OnSchedulerInitialized(L);

    logger->PrintInformation(RbxStu::Scheduler, "Stack popped. Now awaiting execution jobs from the Named Pipe!");
    lua_pop(L, lua_gettop(L));
    lua_pop(rL, lua_gettop(rL));

    if (Communication::GetSingleton()->IsUnsafeMode()) {
        logger->PrintWarning(RbxStu::Scheduler,
                             "WARNING! YOU ARE RUNNING IN UNSAFE MODE! ANY SCRIPT RUN CAN RESULT IN DANGEROUS "
                             "CONSEQUENCES! ONCE YOU FINISH PLAYING AROUND ENABLE SAFE MODE BACK ON!");
    }
}

void Scheduler::ResetScheduler(SchedulerResetReason resetReason) {
    std::lock_guard g{__scheduler_init};
    const auto logger = Logger::GetSingleton();

    if (resetReason == SchedulerResetReason::ExecutionDataModelChanged && this->m_lsRoblox.has_value()) {
        logger->PrintInformation(RbxStu::Scheduler, "Disconnecting scheduler stepping from previous DataModel...");
        const auto L = this->m_lsRoblox.value();
        try {
            lua_getref(L, this->m_lRefToStepConnection);
            lua_getfield(L, -1, "Disconnect");
            lua_pushvalue(L, -2);
            lua_call(L, 1, 1);
            logger->PrintInformation(RbxStu::Scheduler, "Disconnected Scheduler Step!");
        } catch (const std::exception &e) {
            logger->PrintError(RbxStu::Scheduler, std::format("Cannot Disconnect Step! exception -> {}", e.what()));
        }
    }


    this->m_lRefToStepConnection = 0;
    this->m_lsRoblox = {};
    this->m_lsInitialisedWith = {};
    this->m_pClientDataModel = {};

    while (!this->m_qSchedulerJobs.empty()) {
        // Clear job queue
        this->m_qSchedulerJobs.pop();
    }


    if (resetReason == SchedulerResetReason::DataModelLost) {
        logger->PrintInformation(RbxStu::Scheduler, "Applying environment side effects!");
        logger->PrintInformation(RbxStu::Scheduler, "Clearing Luau WebSockets...");
        Websocket::ResetWebsockets(); // Reset websockets

        logger->PrintInformation(RbxStu::Scheduler, "Resetting ClosureManager...");
        ClosureManager::GetSingleton()->ResetManager(this->GetExecutionDataModel());

        logger->PrintInformation(RbxStu::Scheduler,
                                 "Cleaning reference map for the DataModel that is currently initialized...");
        // The DataModel was lost! It is safe to clear our CURRENT ref map!
        RbxStu::s_mRefsMapBasedOnDataModel[this->GetExecutionDataModel()].clear();
    } else {
        logger->PrintInformation(RbxStu::Scheduler, "Cannot apply environment side effects!");
        logger->PrintError(RbxStu::Scheduler, "Cannot reset reference nor Closure map! Doing so will break the "
                                              "previous DataModel ignoring resource collection...");
    }

    logger->PrintInformation(RbxStu::Scheduler, "Scheduler reset completed. All fields set to no value.");
}

bool Scheduler::IsInitialized() const {
    std::lock_guard g{__scheduler_init};
    return this->m_lsInitialisedWith.has_value() && this->m_lsRoblox.has_value();
}
