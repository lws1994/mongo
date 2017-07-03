/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/create_collection.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/repl/replication_coordinator_global.h"

namespace mongo {
namespace {
/**
 * Shared part of the implementation of the createCollection versions for replicated and regular
 * collection creation.
 */
Status createCollection(OperationContext* opCtx,
                        const NamespaceString& nss,
                        const BSONObj& cmdObj,
                        const BSONObj& idIndex,
                        CollectionOptions::ParseKind kind) {
    BSONObjIterator it(cmdObj);

    // Skip the first cmdObj element.
    BSONElement firstElt = it.next();
    invariant(firstElt.fieldNameStringData() == "create");

    Status status = userAllowedCreateNS(nss.db(), nss.coll());
    if (!status.isOK()) {
        return status;
    }

    // Build options object from remaining cmdObj elements.
    BSONObjBuilder optionsBuilder;
    while (it.more()) {
        const auto elem = it.next();
        if (!Command::isGenericArgument(elem.fieldNameStringData()))
            optionsBuilder.append(elem);
    }

    BSONObj options = optionsBuilder.obj();
    uassert(14832,
            "specify size:<n> when capped is true",
            !options["capped"].trueValue() || options["size"].isNumber() ||
                options.hasField("$nExtents"));

    return writeConflictRetry(opCtx, "create", nss.ns(), [&] {
        Lock::DBLock dbXLock(opCtx, nss.db(), MODE_X);
        OldClientContext ctx(opCtx, nss.ns());
        if (opCtx->writesAreReplicated() &&
            !repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(opCtx, nss)) {
            return Status(ErrorCodes::NotMaster,
                          str::stream() << "Not primary while creating collection " << nss.ns());
        }

        WriteUnitOfWork wunit(opCtx);

        // Create collection.
        const bool createDefaultIndexes = true;
        status =
            userCreateNS(opCtx, ctx.db(), nss.ns(), options, kind, createDefaultIndexes, idIndex);

        if (!status.isOK()) {
            return status;
        }

        wunit.commit();

        return Status::OK();
    });
}
}  // namespace

Status createCollection(OperationContext* opCtx,
                        const std::string& dbName,
                        const BSONObj& cmdObj,
                        const BSONObj& idIndex) {
    return createCollection(opCtx,
                            Command::parseNsCollectionRequired(dbName, cmdObj),
                            cmdObj,
                            idIndex,
                            CollectionOptions::parseForCommand);
}

Status createCollectionForApplyOps(OperationContext* opCtx,
                                   const std::string& dbName,
                                   const BSONElement& ui,
                                   const BSONObj& cmdObj,
                                   const BSONObj& idIndex) {
    invariant(opCtx->lockState()->isDbLockedForMode(dbName, MODE_X));
    auto db = dbHolder().get(opCtx, dbName);
    const NamespaceString newCollName(Command::parseNsCollectionRequired(dbName, cmdObj));
    auto newCmd = cmdObj;

    // If a UUID is given, see if we need to rename a collection out of the way, and whether the
    // collection already exists under a different name. If so, rename it into place. As this is
    // done during replay of the oplog, the operations do not need to be atomic, just idempotent.
    // We need to do the renaming part in a separate transaction, as we cannot transactionally
    // create a database on MMAPv1, which could result in createCollection failing if the database
    // does not yet exist.
    if (ui.ok()) {
        // Return an optional, indicating whether we need to early return (if the collection already
        // exists, or in case of an error).
        using Result = boost::optional<Status>;
        auto result =
            writeConflictRetry(opCtx, "createCollectionForApplyOps", newCollName.ns(), [&] {
                WriteUnitOfWork wunit(opCtx);
                // Options need the field to be named "uuid", so parse/recreate.
                auto uuid = uassertStatusOK(UUID::parse(ui));
                uassert(ErrorCodes::InvalidUUID,
                        "Invalid UUID in applyOps create command: " + uuid.toString(),
                        uuid.isRFC4122v4());

                auto& catalog = UUIDCatalog::get(opCtx);
                auto currentName = catalog.lookupNSSByUUID(uuid);
                OpObserver* opObserver = getGlobalServiceContext()->getOpObserver();
                if (currentName == newCollName)
                    return Result(Status::OK());

                // In the case of oplog replay, a future command may have created or renamed a
                // collection with that same name. In that case, renaming this future collection to
                // a
                // random temporary name is correct: once all entries are replayed no temporary
                // names
                // will remain.
                // On MMAPv1 the rename can result in index names that are too long. However this
                // should
                // only happen for initial sync and "resync collection" for rollback, so we can let
                // the
                // error propagate resulting in an abort and restart of the initial sync or result
                // in
                // rollback to fassert, requiring a resync of that node.
                const bool stayTemp = true;
                if (auto futureColl = db ? db->getCollection(opCtx, newCollName) : nullptr) {
                    auto tmpName =
                        NamespaceString(newCollName.db(), "tmp" + UUID::gen().toString());
                    Status status =
                        db->renameCollection(opCtx, newCollName.ns(), tmpName.ns(), stayTemp);
                    if (!status.isOK())
                        return Result(status);
                    opObserver->onRenameCollection(opCtx,
                                                   newCollName,
                                                   tmpName,
                                                   futureColl->uuid(),
                                                   /*dropTarget*/ false,
                                                   /*dropTargetUUID*/ {},
                                                   /*dropSourceUUID*/ {},
                                                   stayTemp);
                }

                // If the collection with the requested UUID already exists, but with a different
                // name,
                // just rename it to 'newCollName'.
                if (catalog.lookupCollectionByUUID(uuid)) {
                    Status status =
                        db->renameCollection(opCtx, currentName.ns(), newCollName.ns(), stayTemp);
                    if (!status.isOK())
                        return Result(status);
                    opObserver->onRenameCollection(opCtx,
                                                   currentName,
                                                   newCollName,
                                                   uuid,
                                                   /*dropTarget*/ false,
                                                   /*dropTargetUUID*/ {},
                                                   /*dropSourceUUID*/ {},
                                                   stayTemp);

                    wunit.commit();
                    return Result(Status::OK());
                }

                // A new collection with the specific UUID must be created, so add the UUID to the
                // creation options. Regular user collection creation commands cannot do this.
                auto uuidObj = uuid.toBSON();
                newCmd = cmdObj.addField(uuidObj.firstElement());
                wunit.commit();

                return Result(boost::none);
            });

        if (result) {
            return *result;
        }
    }

    return createCollection(
        opCtx, newCollName, newCmd, idIndex, CollectionOptions::parseForStorage);
}

}  // namespace mongo
