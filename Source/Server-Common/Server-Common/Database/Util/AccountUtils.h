#pragma once
#include "Server-Common/Database/Definitions.h"

#include <Base/Types.h>

#include <pqxx/pqxx>

namespace Database
{
    struct DBConnection;

    namespace Util::Account
    {
        namespace Loading
        {
            void InitAccountTablesPreparedStatements(std::shared_ptr<DBConnection>& dbConnection);
        }

        bool AccountGetInfoByID(std::shared_ptr<DBConnection>& dbConnection, u64 accountID, pqxx::result& result);
        bool AccountGetInfoByName(std::shared_ptr<DBConnection>& dbConnection, const std::string& name, pqxx::result& result);
        bool AccountCreate(pqxx::work& transaction, const std::string& name, const std::string& email, u64 registrationTimestamp, unsigned char* blob, u32 blobSize, u64& accountID);
        bool AccountDelete(pqxx::work& transaction, u64 accountID);
    }
}