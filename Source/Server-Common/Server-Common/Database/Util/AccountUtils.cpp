#include "AccountUtils.h"
#include "Server-Common/Database/DBController.h"

#include <Base/Util/DebugHandler.h>
#include <Base/Util/StringUtils.h>

#include <pqxx/nontransaction>

namespace Database::Util::Account
{
    namespace Loading
    {
        void InitAccountTablesPreparedStatements(std::shared_ptr<DBConnection>& dbConnection)
        {
            NC_LOG_INFO("Loading Prepared Statements Account Tables...");

            try
            {
                dbConnection->connection->prepare("AccountGetInfoByID", "SELECT * FROM public.accounts WHERE id = $1");
                dbConnection->connection->prepare("AccountGetInfoByName", "SELECT * FROM public.accounts WHERE name = $1");
                dbConnection->connection->prepare("AccountCreate", "INSERT INTO public.accounts (name, email, registration_timestamp, blob) VALUES ($1, $2, $3, $4) RETURNING id");
                dbConnection->connection->prepare("AccountDelete", "DELETE FROM public.accounts WHERE id = $1");
                
                NC_LOG_INFO("Loaded Prepared Statements Account Tables\n");
            }
            catch (const pqxx::sql_error& e)
            {
                NC_LOG_CRITICAL("{0}", e.what());
                return;
            }
        }
    }

    bool AccountGetInfoByID(std::shared_ptr<DBConnection>& dbConnection, u64 accountID, pqxx::result& result)
    {
        try
        {
            pqxx::nontransaction nonTransaction = dbConnection->NewNonTransaction();
            result = nonTransaction.exec(pqxx::prepped("AccountGetInfoByID"), pqxx::params{ accountID });
            if (result.empty())
                return false;

            return true;
        }
        catch (const pqxx::sql_error& e)
        {
            NC_LOG_WARNING("{0}", e.what());
            return false;
        }
    }

    bool AccountGetInfoByName(std::shared_ptr<DBConnection>& dbConnection, const std::string& name, pqxx::result& result)
    {
        try
        {
            pqxx::nontransaction nonTransaction = dbConnection->NewNonTransaction();
            result = nonTransaction.exec(pqxx::prepped("AccountGetInfoByName"), pqxx::params{ name });
            if (result.empty())
                return false;

            return true;
        }
        catch (const pqxx::sql_error& e)
        {
            NC_LOG_WARNING("{0}", e.what());
            return false;
        }
    }

    bool AccountCreate(pqxx::work& transaction, const std::string& name, const std::string& email, u64 registrationTimestamp, unsigned char* blob, u32 blobSize, u64& accountID)
    {
        try
        {
            pqxx::bytes_view binaryBlob = pqxx::binary_cast(blob, blobSize);
            auto queryResult = transaction.exec(pqxx::prepped("AccountCreate"), pqxx::params{ name, email, registrationTimestamp, binaryBlob });
            if (queryResult.empty())
                return false;

            accountID = queryResult[0][0].as<u64>();
            return true;
        }
        catch (const pqxx::sql_error& e)
        {
            NC_LOG_WARNING("{0}", e.what());
            return false;
        }
    }

    bool AccountDelete(pqxx::work& transaction, u64 accountID)
    {
        try
        {
            auto queryResult = transaction.exec(pqxx::prepped("AccountDelete"), pqxx::params{ accountID });
            if (queryResult.affected_rows() == 0)
                return false;

            return true;
        }
        catch (const pqxx::sql_error& e)
        {
            NC_LOG_WARNING("{0}", e.what());
            return false;
        }
    }
}