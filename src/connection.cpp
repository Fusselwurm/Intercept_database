#include "connection.h"
#include <mariadb++/connection.hpp>
#include "query.h"
#include "res.h"
#include "mariadb++/exceptions.hpp"
#include <winsock2.h>
#include "threading.h"
#include "ittnotify.h"

__itt_domain* domainConnection = __itt_domain_create("connection");


using namespace intercept::client;

class GameDataDBConnection : public game_data {

public:
    GameDataDBConnection() {}
    void lastRefDeleted() const override { delete this; }
    const sqf_script_type& type() const override { return Connection::GameDataDBConnection_type; }
    ~GameDataDBConnection() override {};

    bool get_as_bool() const override { return true; }
    float get_as_number() const override { return 0.f; }
    const r_string& get_as_string() const override { static r_string nm("stuff"sv); return nm; }
    game_data* copy() const override { return new GameDataDBConnection(*this); }
    r_string to_string() const override {
        if (!session) return r_string("<no session>"sv);
        if (!session->connected()) return r_string("<not connected>"sv);
        return "<connected to database: " + session->schema() + ">";        
    }
    //virtual bool equals(const game_data*) const override; //#TODO isEqualTo on hashMaps would be quite nice I guess?
    const char* type_as_string() const override { return "databaseConnection"; }
    bool is_nil() const override { return false; }
    bool can_serialize() override { return true; }//Setting this to false causes a fail in scheduled and global vars

    serialization_return serialize(param_archive& ar) override {
        game_data::serialize(ar);
        //size_t entryCount;
        //if (ar._isExporting) entryCount = map.size();
        //ar.serialize("entryCount"sv, entryCount, 1);
        //#TODO add array serialization functions
        //ar._p1->add_array_entry()
        //if (!ar._isExporting) {
        //
        //    for (int i = 0; i < entryCount; ++i) {
        //        s
        //    }
        //
        //
        //
        //}
        return serialization_return::no_error;
    }

    mariadb::connection_ref session;
};

game_data* createGameDataDBConnection(param_archive* ar) {
    auto x = new GameDataDBConnection();
    if (ar)
        x->serialize(*ar);
    return x;
}

game_value Connection::cmd_createConnectionArray(game_state&, game_value_parameter right) {
    //#TODO error checking
    r_string ip = right[0];
    int port = right[1];
    r_string user = right[2];
    r_string pw = right[3];
    r_string db = right[4];

    auto acc = mariadb::account::create(ip, user, pw, db, port);
    

    auto newCon = new GameDataDBConnection();

    newCon->session = mariadb::connection::create(acc);

    return newCon;
}

game_value Connection::cmd_createConnectionConfig(game_state&, game_value_parameter right) {
    
    auto acc = Config::get().getAccount(right);
    if (!acc) return  {};


    auto newCon = new GameDataDBConnection();

    newCon->session = mariadb::connection::create(acc);

    return newCon;
}


class callstack_item_WaitForQueryResult : public vm_context::callstack_item {

public:

    callstack_item_WaitForQueryResult(ref<GameDataDBAsyncResult> inp) : res(inp) {}

    const char* getName() const override { return "stuff"; };
    int varCount() const override { return 0; };
    int getVariables(const IDebugVariable** storage, int count) const override { return 0; };
    types::__internal::I_debug_value::RefType EvaluateExpression(const char* code, unsigned rad) override { return {}; };
    void getSourceDocPosition(char* file, int fileSize, int& line) override {};
    IDebugScope* getParent() override { __debugbreak(); return nullptr; };
    r_string get_type() const override { return "stuff"sv; }




    game_instruction* next(int& d1, const game_state* s) override {
        if (res->data->fut.wait_for(std::chrono::nanoseconds(0)) == std::future_status::ready) {
            
            //push result onto stack.
            auto gd_res = new GameDataDBResult();
            gd_res->res = res->data->res;
            s->get_vm_context()->scriptStack[_stackEndAtStart] = game_value(gd_res);
            d1 = 2; //done
        } else {
            d1 = 3; //wait
        }

        return nullptr;
    };
    bool someEH(void* state) override { return false; };
    bool someEH2(void* state) override { return false; };
    void on_before_exec() override {
        
    };

    ref<GameDataDBAsyncResult> res;


};

game_value Connection::cmd_execute(game_state& gs, game_value_parameter con, game_value_parameter qu) {
    auto session = con.get_as<GameDataDBConnection>()->session;
    auto query = qu.get_as<GameDataDBQuery>();

    if (!gs.get_vm_context()->is_scheduled()) {

        auto statement = session->create_statement(query->getQueryString());

        uint32_t idx = 0;
        for (auto& it : query->boundValues) {

            switch (it.type_enum()) {
            case game_data_type::SCALAR: statement->set_float(idx++, static_cast<float>(it)); break;
            case game_data_type::BOOL: statement->set_boolean(idx++, static_cast<bool>(it)); break;
            case game_data_type::STRING: statement->set_string(idx++, static_cast<r_string>(it)); break;
            default:;
            }
        }

        auto res = statement->query();

        auto gd_res = new GameDataDBResult();
        gd_res->res = res;

        return gd_res;
    }
    //Set up callstack item to suspend while waiting

    auto& cs = gs.get_vm_context()->callstack;

    auto gd_res = new GameDataDBAsyncResult();
    gd_res->data = std::make_shared<GameDataDBAsyncResult::dataT>();



    gd_res->data->fut = Threading::get().pushTask(session,
        [stmt = query->getQueryString(), boundV = query->boundValues, result = gd_res->data](mariadb::connection_ref con) -> bool
    {
        try {
            auto statement = con->create_statement(stmt);
            uint32_t idx = 0;
            for (auto& it : boundV) {

                switch (it.type_enum()) {
                case game_data_type::SCALAR: statement->set_float(idx++, static_cast<float>(it)); break;
                case game_data_type::BOOL: statement->set_boolean(idx++, static_cast<bool>(it)); break;
                case game_data_type::STRING: statement->set_string(idx++, static_cast<r_string>(it)); break;
                }
            }
            result->res = statement->query();
            return true;
        }
        catch (mariadb::exception::connection& x) {
            auto exText = r_string("Intercept-DB exception ") + x.what() + "\nat\n" + stmt;
            invoker_lock l();
            sqf::diag_log(exText);

            return false;
        }
    });

    auto newItem = new callstack_item_WaitForQueryResult(gd_res);
    newItem->_parent = cs.back();
    newItem->_stackEndAtStart = gs.get_vm_context()->scriptStack.size()-2;
    newItem->_stackEnd = newItem->_stackEndAtStart+1;
    newItem->_varSpace.parent = &cs.back()->_varSpace;
    cs.emplace_back(newItem);
    return 123;
}

__itt_string_handle* connection_cmd_executeAsync = __itt_string_handle_create("Connection::cmd_executeAsync");
__itt_string_handle* connection_cmd_executeAsync_task = __itt_string_handle_create("Connection::cmd_executeAsync::task");

game_value Connection::cmd_executeAsync(game_state&, game_value_parameter con, game_value_parameter qu) {
    __itt_task_begin(domainConnection, __itt_null, __itt_null, connection_cmd_executeAsync);
    auto session = con.get_as<GameDataDBConnection>()->session;
    auto query = qu.get_as<GameDataDBQuery>();

    auto gd_res = new GameDataDBAsyncResult();
    gd_res->data = std::make_shared<GameDataDBAsyncResult::dataT>();
    gd_res->data->fut = Threading::get().pushTask(session,   
    [stmt = query->getQueryString(), boundV = query->boundValues, result = gd_res->data](mariadb::connection_ref con) -> bool
    {
        __itt_task_begin(domainConnection, __itt_null, __itt_null, connection_cmd_executeAsync_task);
        try {
            auto statement = con->create_statement(stmt);
            uint32_t idx = 0;
            for (auto& it : boundV) {

                switch (it.type_enum()) {
                case game_data_type::SCALAR: statement->set_float(idx++, static_cast<float>(it)); break;
                case game_data_type::BOOL: statement->set_boolean(idx++, static_cast<bool>(it)); break;
                case game_data_type::STRING: statement->set_string(idx++, static_cast<r_string>(it)); break;
                }
            }
            result->res = statement->query();
            __itt_task_end(domainConnection);
            return true;
        } catch (mariadb::exception::connection& x) {
            auto exText = r_string("Intercept-DB exception ") + x.what() + "\nat\n" + stmt;
            invoker_lock l();
            sqf::diag_log(exText);

            __itt_task_end(domainConnection);
            return false;
        }
    }, true);
    Threading::get().pushAsyncWork(gd_res);
    __itt_task_end(domainConnection);
    return gd_res;
}

game_value Connection::cmd_ping(game_state&, game_value_parameter con) {
    try {
        auto session = con.get_as<GameDataDBConnection>()->session;
        return session->query("SELECT 1;"sv)->get_signed8(0) == 1;
     } catch (mariadb::exception::connection& x) {
        auto exText = r_string("Intercept-DB ping exception ") + x.what();
        sqf::diag_log(exText);
        return false;
    }
}

void Connection::initCommands() {
    
    auto dbType = host::register_sqf_type("DBCON"sv, "databaseConnection"sv, "TODO"sv, "databaseConnection"sv, createGameDataDBConnection);
    GameDataDBConnection_typeE = dbType.first;
    GameDataDBConnection_type = dbType.second;


    handle_cmd_createConnection = host::register_sqf_command("dbCreateConnection", "TODO", Connection::cmd_createConnectionArray, GameDataDBConnection_typeE, game_data_type::ARRAY);
    handle_cmd_createConnectionConfig = host::register_sqf_command("dbCreateConnection", "TODO", Connection::cmd_createConnectionConfig, GameDataDBConnection_typeE, game_data_type::STRING);
    handle_cmd_execute = host::register_sqf_command("dbExecute", "TODO", Connection::cmd_execute, Result::GameDataDBResult_typeE, GameDataDBConnection_typeE, Query::GameDataDBQuery_typeE);
    handle_cmd_executeAsync = host::register_sqf_command("dbExecuteAsync", "TODO", Connection::cmd_executeAsync, Result::GameDataDBAsyncResult_typeE, GameDataDBConnection_typeE, Query::GameDataDBQuery_typeE);
    handle_cmd_ping = host::register_sqf_command("dbPing", "TODO", Connection::cmd_ping, game_data_type::ARRAY, GameDataDBConnection_typeE);
}
