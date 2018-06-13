/*
 * masterserver.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "flow/actorcompiler.h"
#include "flow/ActorCollection.h"
#include "fdbrpc/PerfMetric.h"
#include "flow/Trace.h"
#include "fdbrpc/FailureMonitor.h"
#include "fdbclient/NativeAPI.h"
#include "fdbclient/Notified.h"
#include "fdbclient/SystemData.h"
#include "ConflictSet.h"
#include "DataDistribution.h"
#include "Knobs.h"
#include <iterator>
#include "WaitFailure.h"
#include "WorkerInterface.h"
#include "Ratekeeper.h"
#include "ClusterRecruitmentInterface.h"
#include "ServerDBInfo.h"
#include "CoordinatedState.h"
#include "fdbserver/CoordinationInterface.h"  // copy constructors for ServerCoordinators class
#include "fdbrpc/sim_validation.h"
#include "DBCoreState.h"
#include "LogSystem.h"
#include "LogSystemDiskQueueAdapter.h"
#include "IKeyValueStore.h"
#include "ApplyMetadataMutation.h"
#include "RecoveryState.h"

using std::vector;
using std::min;
using std::max;

struct ProxyVersionReplies {
	std::map<uint64_t, GetCommitVersionReply> replies;
	NotifiedVersion latestRequestNum;

	ProxyVersionReplies(ProxyVersionReplies&& r) noexcept(true)  : replies(std::move(r.replies)), latestRequestNum(std::move(r.latestRequestNum)) {}
	void operator=(ProxyVersionReplies&& r) noexcept(true) { replies = std::move(r.replies); latestRequestNum = std::move(r.latestRequestNum); }

	ProxyVersionReplies() : latestRequestNum(0) {}
};

ACTOR Future<Void> masterTerminateOnConflict( UID dbgid, Promise<Void> fullyRecovered, Future<Void> onConflict, Future<Void> switchedState ) {
	choose {
		when( Void _ = wait(onConflict) ) {
			if (!fullyRecovered.isSet()) {
				TraceEvent("MasterTerminated", dbgid).detail("Reason", "Conflict");
				TEST(true);  // Coordinated state conflict, master dying
				throw worker_removed();
			}
			return Void();
		}
		when( Void _ = wait(switchedState) ) {
			return Void();
		}
	}
}

class ReusableCoordinatedState : NonCopyable {
public:
	Promise<Void> fullyRecovered;
	DBCoreState prevDBState;
	DBCoreState myDBState;
	bool finalWriteStarted;
	Future<Void> previousWrite;

	ReusableCoordinatedState( ServerCoordinators const& coordinators, PromiseStream<Future<Void>> const& addActor, UID const& dbgid ) : coordinators(coordinators), cstate(coordinators), addActor(addActor), dbgid(dbgid), finalWriteStarted(false), previousWrite(Void()) {}

	Future<Void> read() {
		return _read(this);
	}

	Future<Void> write(DBCoreState newState, bool finalWrite = false) {
		previousWrite = _write(this, newState, finalWrite);
		return previousWrite;
	}

	Future<Void> move( ClusterConnectionString const& nc ) {
		return cstate.move(nc);
	}

private:
	MovableCoordinatedState cstate;
	ServerCoordinators coordinators;
	PromiseStream<Future<Void>> addActor;
	Promise<Void> switchedState;
	UID dbgid;

	ACTOR Future<Void> _read(ReusableCoordinatedState* self) {
		Value prevDBStateRaw = wait( self->cstate.read() );
		Future<Void> onConflict = masterTerminateOnConflict( self->dbgid, self->fullyRecovered, self->cstate.onConflict(), self->switchedState.getFuture() );
		if(onConflict.isReady() && onConflict.isError()) {
			throw onConflict.getError();
		}
		self->addActor.send( onConflict );

		if( prevDBStateRaw.size() ) {
			self->prevDBState = BinaryReader::fromStringRef<DBCoreState>(prevDBStateRaw, IncludeVersion());
			self->myDBState = self->prevDBState;
		}

		return Void();
	}

	ACTOR Future<Void> _write(ReusableCoordinatedState* self, DBCoreState newState, bool finalWrite) {
		if(self->finalWriteStarted) {
			Void _ = wait( Future<Void>(Never()) );
		}

		if(finalWrite) {
			self->finalWriteStarted = true;
		}
		
		try {
			Void _ = wait( self->cstate.setExclusive( BinaryWriter::toValue(newState, IncludeVersion()) ) );
		} catch (Error& e) {
			TEST(true); // Master displaced during writeMasterState
			throw;
		}

		self->myDBState = newState;

		if(!finalWrite) {
			self->switchedState.send(Void());
			self->cstate = MovableCoordinatedState(self->coordinators);
			Value rereadDBStateRaw = wait( self->cstate.read() );
			DBCoreState readState;
			if( rereadDBStateRaw.size() )
				readState = BinaryReader::fromStringRef<DBCoreState>(rereadDBStateRaw, IncludeVersion());

			if( readState != newState ) {
				TraceEvent("MasterTerminated", self->dbgid).detail("Reason", "CStateChanged");
				TEST(true);  // Coordinated state changed between writing and reading, master dying
				throw worker_removed();
			}
			self->switchedState = Promise<Void>();
			self->addActor.send( masterTerminateOnConflict( self->dbgid, self->fullyRecovered, self->cstate.onConflict(), self->switchedState.getFuture() ) );
		} else {
			self->fullyRecovered.send(Void());
		}
		
		return Void();
	}
};

struct MasterData : NonCopyable, ReferenceCounted<MasterData> {
	UID dbgid;

	AsyncTrigger registrationTrigger;
	Version lastEpochEnd, // The last version in the old epoch not (to be) rolled back in this recovery
		recoveryTransactionVersion;  // The first version in this epoch
	double lastCommitTime;

	DatabaseConfiguration originalConfiguration;
	DatabaseConfiguration configuration;
	std::vector<Optional<Key>> primaryDcId;
	std::vector<Optional<Key>> remoteDcIds;
	bool hasConfiguration;

	ServerCoordinators coordinators;

	Reference< ILogSystem > logSystem;
	Version version;   // The last version assigned to a proxy by getVersion()
	double lastVersionTime;
	LogSystemDiskQueueAdapter* txnStateLogAdapter;
	IKeyValueStore* txnStateStore;
	int64_t memoryLimit;
	std::map<Optional<Value>,int8_t> dcId_locality;
	std::vector<Tag> allTags;

	int8_t getNextLocality() {
		int8_t maxLocality = -1;
		for(auto it : dcId_locality) {
			maxLocality = std::max(maxLocality, it.second);
		}
		return maxLocality + 1;
	}

	vector< MasterProxyInterface > proxies;
	vector< MasterProxyInterface > provisionalProxies;
	vector< ResolverInterface > resolvers;

	std::map<UID, ProxyVersionReplies> lastProxyVersionReplies;

	Standalone<StringRef> dbName;
	Standalone<StringRef> dbId;

	MasterInterface myInterface;
	ClusterControllerFullInterface clusterController;  // If the cluster controller changes, this master will die, so this is immutable.

	ReusableCoordinatedState cstate;
	Promise<Void> cstateUpdated;
	Reference<AsyncVar<ServerDBInfo>> dbInfo;
	int64_t registrationCount; // Number of different MasterRegistrationRequests sent to clusterController

	RecoveryState recoveryState;

	AsyncVar<Standalone<VectorRef<ResolverMoveRef>>> resolverChanges;
	Version resolverChangesVersion;
	std::set<UID> resolverNeedingChanges;

	PromiseStream<Future<Void>> addActor;

	MasterData(
		Reference<AsyncVar<ServerDBInfo>> const& dbInfo,
		MasterInterface const& myInterface,
		ServerCoordinators const& coordinators,
		ClusterControllerFullInterface const& clusterController,
		Standalone<StringRef> const& dbName,
		Standalone<StringRef> const& dbId,
		PromiseStream<Future<Void>> const& addActor
		)
		: dbgid(myInterface.id()),
		  myInterface(myInterface),
		  dbInfo(dbInfo),
		  cstate(coordinators, addActor, dbgid),
		  coordinators(coordinators),
		  clusterController(clusterController),
		  dbName(dbName),
		  dbId(dbId),
		  lastEpochEnd(invalidVersion),
		  recoveryTransactionVersion(invalidVersion),
		  lastCommitTime(0),
		  registrationCount(0),
		  version(invalidVersion),
		  lastVersionTime(0),
		  txnStateStore(0),
		  memoryLimit(2e9),
		  addActor(addActor),
		  hasConfiguration(false)
	{
	}
	~MasterData() { if(txnStateStore) txnStateStore->close(); }
};

ACTOR Future<Void> newProxies( Reference<MasterData> self, RecruitFromConfigurationReply recr ) {
	vector<Future<MasterProxyInterface>> initializationReplies;
	for( int i = 0; i < recr.proxies.size(); i++ ) {
		InitializeMasterProxyRequest req;
		req.master = self->myInterface;
		req.recoveryCount = self->cstate.myDBState.recoveryCount + 1;
		req.recoveryTransactionVersion = self->recoveryTransactionVersion;
		req.firstProxy = i == 0;
		TraceEvent("ProxyReplies",self->dbgid).detail("WorkerID", recr.proxies[i].id());
		initializationReplies.push_back( transformErrors( throwErrorOr( recr.proxies[i].masterProxy.getReplyUnlessFailedFor( req, SERVER_KNOBS->TLOG_TIMEOUT, SERVER_KNOBS->MASTER_FAILURE_SLOPE_DURING_RECOVERY ) ), master_recovery_failed() ) );
	}

	vector<MasterProxyInterface> newRecruits = wait( getAll( initializationReplies ) );
	// It is required for the correctness of COMMIT_ON_FIRST_PROXY that self->proxies[0] is the firstProxy.
	self->proxies = newRecruits;

	return Void();
}

ACTOR Future<Void> newResolvers( Reference<MasterData> self, RecruitFromConfigurationReply recr ) {
	vector<Future<ResolverInterface>> initializationReplies;
	for( int i = 0; i < recr.resolvers.size(); i++ ) {
		InitializeResolverRequest req;
		req.recoveryCount = self->cstate.myDBState.recoveryCount + 1;
		req.proxyCount = recr.proxies.size();
		req.resolverCount = recr.resolvers.size();
		TraceEvent("ResolverReplies",self->dbgid).detail("WorkerID", recr.resolvers[i].id());
		initializationReplies.push_back( transformErrors( throwErrorOr( recr.resolvers[i].resolver.getReplyUnlessFailedFor( req, SERVER_KNOBS->TLOG_TIMEOUT, SERVER_KNOBS->MASTER_FAILURE_SLOPE_DURING_RECOVERY ) ), master_recovery_failed() ) );
	}

	vector<ResolverInterface> newRecruits = wait( getAll( initializationReplies ) );
	self->resolvers = newRecruits;

	return Void();
}

ACTOR Future<Void> newTLogServers( Reference<MasterData> self, RecruitFromConfigurationReply recr, Reference<ILogSystem> oldLogSystem, vector<Standalone<CommitTransactionRef>>* initialConfChanges ) {
	if(self->configuration.remoteTLogReplicationFactor > 0) {
		state Optional<Key> remoteDcId = self->remoteDcIds.size() ? self->remoteDcIds[0] : Optional<Key>();
		if( !self->dcId_locality.count(recr.dcId) ) {
			TraceEvent(SevWarn, "UnknownPrimaryDCID", self->dbgid).detail("PrimaryId", printable(recr.dcId));
			int8_t loc = self->getNextLocality();
			Standalone<CommitTransactionRef> tr;
			tr.set(tr.arena(), tagLocalityListKeyFor(recr.dcId), tagLocalityListValue(loc));
			initialConfChanges->push_back(tr);
			self->dcId_locality[recr.dcId] = loc;
		}

		if( !self->dcId_locality.count(remoteDcId) ) {
			TraceEvent(SevWarn, "UnknownRemoteDCID", self->dbgid).detail("RemoteId", printable(remoteDcId));
			int8_t loc = self->getNextLocality();
			Standalone<CommitTransactionRef> tr;
			tr.set(tr.arena(), tagLocalityListKeyFor(remoteDcId), tagLocalityListValue(loc));
			initialConfChanges->push_back(tr);
			self->dcId_locality[remoteDcId] = loc;
		}

		Future<RecruitRemoteFromConfigurationReply> fRemoteWorkers = brokenPromiseToNever( self->clusterController.recruitRemoteFromConfiguration.getReply( RecruitRemoteFromConfigurationRequest( self->configuration, remoteDcId, recr.tLogs.size() ) ) );

		Reference<ILogSystem> newLogSystem = wait( oldLogSystem->newEpoch( recr, fRemoteWorkers, self->configuration, self->cstate.myDBState.recoveryCount + 1, self->dcId_locality[recr.dcId], self->dcId_locality[remoteDcId], self->allTags ) );
		self->logSystem = newLogSystem;
	} else {
		Reference<ILogSystem> newLogSystem = wait( oldLogSystem->newEpoch( recr, Never(), self->configuration, self->cstate.myDBState.recoveryCount + 1, tagLocalitySpecial, tagLocalitySpecial, self->allTags ) );
		self->logSystem = newLogSystem;
	}
	return Void();
}

ACTOR Future<Void> newSeedServers( Reference<MasterData> self, RecruitFromConfigurationReply recruits, vector<StorageServerInterface>* servers ) {
	// This is only necessary if the database is at version 0
	servers->clear();
	if (self->lastEpochEnd) return Void();

	state int idx = 0;
	state std::map<Optional<Value>, Tag> dcId_tags;
	state int8_t nextLocality = 0;
	while( idx < recruits.storageServers.size() ) {
		TraceEvent("MasterRecruitingInitialStorageServer", self->dbgid)
			.detail("CandidateWorker", recruits.storageServers[idx].locality.toString());

		InitializeStorageRequest isr;
		isr.seedTag = dcId_tags.count(recruits.storageServers[idx].locality.dcId()) ? dcId_tags[recruits.storageServers[idx].locality.dcId()] : Tag(nextLocality, 0);
		isr.storeType = self->configuration.storageServerStoreType;
		isr.reqId = g_random->randomUniqueID();
		isr.interfaceId = g_random->randomUniqueID();

		ErrorOr<InitializeStorageReply> newServer = wait( recruits.storageServers[idx].storage.tryGetReply( isr ) );

		if( newServer.isError() ) {
			if( !newServer.isError( error_code_recruitment_failed ) && !newServer.isError( error_code_request_maybe_delivered ) )
				throw newServer.getError();

			TEST( true ); // masterserver initial storage recuitment loop failed to get new server
			Void _ = wait( delay(SERVER_KNOBS->STORAGE_RECRUITMENT_DELAY) );
		}
		else {
			if(!dcId_tags.count(recruits.storageServers[idx].locality.dcId())) {
				dcId_tags[recruits.storageServers[idx].locality.dcId()] = Tag(nextLocality, 0);
				nextLocality++;
			}
				
			Tag& tag = dcId_tags[recruits.storageServers[idx].locality.dcId()];
			tag.id++;
			idx++;

			servers->push_back( newServer.get().interf );
		}
	}

	self->dcId_locality.clear();
	for(auto& it : dcId_tags) {
		self->dcId_locality[it.first] = it.second.locality;
	}

	TraceEvent("MasterRecruitedInitialStorageServers", self->dbgid)
			.detail("TargetCount", self->configuration.storageTeamSize)
			.detail("Servers", describe(*servers));

	return Void();
}

Future<Void> waitProxyFailure( vector<MasterProxyInterface> const& proxies ) {
	vector<Future<Void>> failed;
	for(int i=0; i<proxies.size(); i++)
		failed.push_back( waitFailureClient( proxies[i].waitFailure, SERVER_KNOBS->TLOG_TIMEOUT, -SERVER_KNOBS->TLOG_TIMEOUT/SERVER_KNOBS->SECONDS_BEFORE_NO_FAILURE_DELAY ) );
	ASSERT( failed.size() >= 1 );
	return tagError<Void>(quorum( failed, 1 ), master_proxy_failed());
}

Future<Void> waitResolverFailure( vector<ResolverInterface> const& resolvers ) {
	vector<Future<Void>> failed;
	for(int i=0; i<resolvers.size(); i++)
		failed.push_back( waitFailureClient( resolvers[i].waitFailure, SERVER_KNOBS->TLOG_TIMEOUT, -SERVER_KNOBS->TLOG_TIMEOUT/SERVER_KNOBS->SECONDS_BEFORE_NO_FAILURE_DELAY ) );
	ASSERT( failed.size() >= 1 );
	return tagError<Void>(quorum( failed, 1 ), master_resolver_failed());
}

ACTOR Future<Void> updateLogsValue( Reference<MasterData> self, Database cx ) {
	state Transaction tr(cx);
	loop {
		try {
			Optional<Standalone<StringRef>> value = wait( tr.get(logsKey) );
			ASSERT(value.present());

			std::vector<OptionalInterface<TLogInterface>> logConf;
			auto logs = decodeLogsValue(value.get());
			for(auto& log : self->logSystem->getLogSystemConfig().tLogs) {
				for(auto& tl : log.tLogs) {
					logConf.push_back(tl);
				}
			}

			bool match = (logs.first.size() == logConf.size());
			if(match) {
				for(int i = 0; i < logs.first.size(); i++) {
					if(logs.first[i].first != logConf[i].id()) {
						match = false;
						break;
					}
				}
			}

			if(!match) {
				TEST(true); //old master attempted to change logsKey
				return Void();
			}

			//FIXME: include remote logs in the log key
			tr.set(logsKey, self->logSystem->getLogsValue());
			Void _ = wait( tr.commit() );
			return Void();
		} catch( Error &e ) {
			Void _ = wait( tr.onError(e) );
		}
	}
}

Future<Void> sendMasterRegistration( MasterData* self, LogSystemConfig const& logSystemConfig, vector<MasterProxyInterface> proxies, vector<ResolverInterface> resolvers, DBRecoveryCount recoveryCount, vector<UID> priorCommittedLogServers ) {
	RegisterMasterRequest masterReq;
	masterReq.dbName = self->dbName;
	masterReq.id = self->myInterface.id();
	masterReq.mi = self->myInterface.locality;
	masterReq.logSystemConfig = logSystemConfig;
	masterReq.proxies = proxies;
	masterReq.resolvers = resolvers;
	masterReq.recoveryCount = recoveryCount;
	if(self->hasConfiguration) masterReq.configuration = self->configuration;
	masterReq.registrationCount = ++self->registrationCount;
	masterReq.priorCommittedLogServers = priorCommittedLogServers;
	masterReq.recoveryState = self->recoveryState;
	return brokenPromiseToNever( self->clusterController.registerMaster.getReply( masterReq ) );
}

ACTOR Future<Void> updateRegistration( Reference<MasterData> self, Reference<ILogSystem> logSystem ) {
	state Database cx = openDBOnServer(self->dbInfo, TaskDefaultEndpoint, true, true);
	state Future<Void> trigger =  self->registrationTrigger.onTrigger();
	state Future<Void> updateLogsKey;

	loop {
		Void _ = wait( trigger );
		Void _ = wait( delay( .001 ) );  // Coalesce multiple changes

		trigger =  self->registrationTrigger.onTrigger();

		TraceEvent("MasterUpdateRegistration", self->dbgid).detail("RecoveryCount", self->cstate.myDBState.recoveryCount).detail("Logs", describe(logSystem->getLogSystemConfig().tLogs));

		if (!self->cstateUpdated.isSet()) {
			Void _ = wait(sendMasterRegistration(self.getPtr(), logSystem->getLogSystemConfig(), self->provisionalProxies, self->resolvers, self->cstate.myDBState.recoveryCount, self->cstate.prevDBState.getPriorCommittedLogServers() ));
		} else {
			updateLogsKey = updateLogsValue(self, cx);
			Void _ = wait( sendMasterRegistration( self.getPtr(), logSystem->getLogSystemConfig(), self->proxies, self->resolvers, self->cstate.myDBState.recoveryCount, vector<UID>() ) );
		}
	}
}

ACTOR Future<Standalone<CommitTransactionRef>> provisionalMaster( Reference<MasterData> parent, Future<Void> activate ) {
	Void _ = wait(activate);

	// Register a fake master proxy (to be provided right here) to make ourselves available to clients
	parent->provisionalProxies = vector<MasterProxyInterface>(1);
	parent->provisionalProxies[0].locality = parent->myInterface.locality;
	state Future<Void> waitFailure = waitFailureServer(parent->provisionalProxies[0].waitFailure.getFuture());
	parent->registrationTrigger.trigger();

	auto lockedKey = parent->txnStateStore->readValue(databaseLockedKey).get();
	state bool locked = lockedKey.present() && lockedKey.get().size();

	// We respond to a minimal subset of the master proxy protocol.  Our sole purpose is to receive a single write-only transaction
	// which might repair our configuration, and return it.
	loop choose {
		when ( GetReadVersionRequest req = waitNext( parent->provisionalProxies[0].getConsistentReadVersion.getFuture() ) ) {
			if ( req.flags & GetReadVersionRequest::FLAG_CAUSAL_READ_RISKY && parent->lastEpochEnd ) {
				GetReadVersionReply rep;
				rep.version = parent->lastEpochEnd;
				rep.locked = locked;
				req.reply.send( rep );
			} else
				req.reply.send(Never());  // We can't perform causally consistent reads without recovering
		}
		when ( CommitTransactionRequest req = waitNext( parent->provisionalProxies[0].commit.getFuture() ) ) {
			req.reply.send(Never()); // don't reply (clients always get commit_unknown_result)
			auto t = &req.transaction;
			TraceEvent("PM_CTC", parent->dbgid).detail("Snapshot", t->read_snapshot).detail("Now", parent->lastEpochEnd);
			if (t->read_snapshot == parent->lastEpochEnd && //< So no transactions can fall between the read snapshot and the recovery transaction this (might) be merged with
				// vvv and also the changes we will make in the recovery transaction (most notably to lastEpochEndKey) BEFORE we merge initialConfChanges won't conflict
				!std::any_of(t->read_conflict_ranges.begin(), t->read_conflict_ranges.end(), [](KeyRangeRef const& r){return r.contains(lastEpochEndKey);}))
			{
				for(auto m = t->mutations.begin(); m != t->mutations.end(); ++m) {
					TraceEvent("PM_CTM", parent->dbgid).detail("MType", m->type).detail("Param1", printable(m->param1)).detail("Param2", printable(m->param2));
					if (isMetadataMutation(*m)) {
						// We keep the mutations and write conflict ranges from this transaction, but not its read conflict ranges
						Standalone<CommitTransactionRef> out;
						out.read_snapshot = invalidVersion;
						out.mutations.append_deep(out.arena(), t->mutations.begin(), t->mutations.size());
						out.write_conflict_ranges.append_deep(out.arena(), t->write_conflict_ranges.begin(), t->write_conflict_ranges.size());
						return out;
					}
				}
			}
		}
		when ( GetKeyServerLocationsRequest req = waitNext( parent->provisionalProxies[0].getKeyServersLocations.getFuture() ) ) {
			req.reply.send(Never());
		}
		when ( Void _ = wait( waitFailure ) ) { throw worker_removed(); }
	}
}

ACTOR Future<Void> recruitEverything( Reference<MasterData> self, vector<StorageServerInterface>* seedServers, Reference<ILogSystem> oldLogSystem, vector<Standalone<CommitTransactionRef>>* initialConfChanges ) {
	if (!self->configuration.isValid()) {
		RecoveryStatus::RecoveryStatus status;
		if (self->configuration.initialized)
			status = RecoveryStatus::configuration_invalid;
		else if (!self->cstate.prevDBState.tLogs.size())
			status = RecoveryStatus::configuration_never_created;
		else
			status = RecoveryStatus::configuration_missing;
		TraceEvent("MasterRecoveryState", self->dbgid)
			.detail("StatusCode", status)
			.detail("Status", RecoveryStatus::names[status])
			.trackLatest("MasterRecoveryState");
		return Never();
	} else
		TraceEvent("MasterRecoveryState", self->dbgid)
			.detail("StatusCode", RecoveryStatus::recruiting_transaction_servers)
			.detail("Status", RecoveryStatus::names[RecoveryStatus::recruiting_transaction_servers])
			.detail("RequiredTLogs", self->configuration.tLogReplicationFactor)
			.detail("DesiredTLogs", self->configuration.getDesiredLogs())
			.detail("RequiredProxies", 1)
			.detail("DesiredProxies", self->configuration.getDesiredProxies())
			.detail("RequiredResolvers", 1)
			.detail("DesiredResolvers", self->configuration.getDesiredResolvers())
			.detail("StoreType", self->configuration.storageServerStoreType)
			.trackLatest("MasterRecoveryState");
	
	//FIXME: we only need log routers for the same locality as the master
	int maxLogRouters = self->cstate.prevDBState.logRouterTags;
	for(auto& old : self->cstate.prevDBState.oldTLogData) {
		maxLogRouters = std::max(maxLogRouters, old.logRouterTags);
	}

	state RecruitFromConfigurationReply recruits = wait(
		brokenPromiseToNever( self->clusterController.recruitFromConfiguration.getReply(
			RecruitFromConfigurationRequest( self->configuration, self->lastEpochEnd==0, maxLogRouters ) ) ) );

	self->primaryDcId.clear();
	self->remoteDcIds.clear();
	if(recruits.dcId.present()) {
		self->primaryDcId.push_back(recruits.dcId);
		if(self->configuration.regions.size() > 1) {
			self->remoteDcIds.push_back(recruits.dcId.get() == self->configuration.regions[0].dcId ? self->configuration.regions[1].dcId : self->configuration.regions[0].dcId);
		}
	}
	
	TraceEvent("MasterRecoveryState", self->dbgid)
		.detail("StatusCode", RecoveryStatus::initializing_transaction_servers)
		.detail("Status", RecoveryStatus::names[RecoveryStatus::initializing_transaction_servers])
		.detail("Proxies", recruits.proxies.size())
		.detail("TLogs", recruits.tLogs.size())
		.detail("Resolvers", recruits.resolvers.size())
		.trackLatest("MasterRecoveryState");

	// Actually, newSeedServers does both the recruiting and initialization of the seed servers; so if this is a brand new database we are sort of lying that we are
	// past the recruitment phase.  In a perfect world we would split that up so that the recruitment part happens above (in parallel with recruiting the transaction servers?).
	Void _ = wait( newSeedServers( self, recruits, seedServers ) );
	Void _ = wait( newProxies( self, recruits ) && newResolvers( self, recruits ) && newTLogServers( self, recruits, oldLogSystem, initialConfChanges ) );
	return Void();
}

ACTOR Future<Void> readTransactionSystemState( Reference<MasterData> self, Reference<ILogSystem> oldLogSystem ) {
	// Peek the txnStateTag in oldLogSystem and recover self->txnStateStore

	// For now, we also obtain the recovery metadata that the log system obtained during the end_epoch process for comparison

	// Sets self->lastEpochEnd and self->recoveryTransactionVersion
	// Sets self->configuration to the configuration (FF/conf/ keys) at self->lastEpochEnd

	// Recover transaction state store
	if(self->txnStateStore) self->txnStateStore->close();
	self->txnStateLogAdapter = openDiskQueueAdapter( oldLogSystem, txsTag );
	self->txnStateStore = keyValueStoreLogSystem( self->txnStateLogAdapter, self->dbgid, self->memoryLimit, false, false );

	// Versionstamped operations (particularly those applied from DR) define a minimum commit version
	// that we may recover to, as they embed the version in user-readable data and require that no
	// transactions will be committed at a lower version.
	Optional<Standalone<StringRef>> requiredCommitVersion = wait(self->txnStateStore->readValue( minRequiredCommitVersionKey ));
	Version minRequiredCommitVersion = -1;
	if (requiredCommitVersion.present()) {
		minRequiredCommitVersion = BinaryReader::fromStringRef<Version>(requiredCommitVersion.get(), Unversioned());
	}

	// Recover version info
	self->lastEpochEnd = oldLogSystem->getEnd() - 1;
	if (self->lastEpochEnd == 0) {
		self->recoveryTransactionVersion = 1;
	} else {
		self->recoveryTransactionVersion = self->lastEpochEnd + SERVER_KNOBS->MAX_VERSIONS_IN_FLIGHT;
		if(BUGGIFY) {
			self->recoveryTransactionVersion += g_random->randomInt64(0, SERVER_KNOBS->MAX_VERSIONS_IN_FLIGHT);
		}
		if ( self->recoveryTransactionVersion < minRequiredCommitVersion ) self->recoveryTransactionVersion = minRequiredCommitVersion;
	}

	TraceEvent("MasterRecovering", self->dbgid).detail("LastEpochEnd", self->lastEpochEnd).detail("RecoveryTransactionVersion", self->recoveryTransactionVersion);

	Standalone<VectorRef<KeyValueRef>> rawConf = wait( self->txnStateStore->readRange( configKeys ) );
	self->configuration.fromKeyValues( rawConf );
	self->originalConfiguration = self->configuration;
	self->hasConfiguration = true;
	TraceEvent("MasterRecoveredConfig", self->dbgid).detail("Conf", self->configuration.toString()).trackLatest("RecoveredConfig");

	Standalone<VectorRef<KeyValueRef>> rawLocalities = wait( self->txnStateStore->readRange( tagLocalityListKeys ) );
	self->dcId_locality.clear();
	for(auto& kv : rawLocalities) {
		self->dcId_locality[decodeTagLocalityListKey(kv.key)] = decodeTagLocalityListValue(kv.value);
	}

	Standalone<VectorRef<KeyValueRef>> rawTags = wait( self->txnStateStore->readRange( serverTagKeys ) );
	self->allTags.clear();
	self->allTags.push_back(txsTag);
	for(auto& kv : rawTags) {
		self->allTags.push_back(decodeServerTagValue( kv.value ));
	}

	Standalone<VectorRef<KeyValueRef>> rawHistoryTags = wait( self->txnStateStore->readRange( serverTagHistoryKeys ) );
	for(auto& kv : rawHistoryTags) {
		self->allTags.push_back(decodeServerTagValue( kv.value ));
	}

	uniquify(self->allTags);

	//auto kvs = self->txnStateStore->readRange( systemKeys );
	//for( auto & kv : kvs.get() )
	//	TraceEvent("MasterRecoveredTXS", self->dbgid).detail("K", printable(kv.key)).detail("V", printable(kv.value));

	self->txnStateLogAdapter->setNextVersion( oldLogSystem->getEnd() );  //< FIXME: (1) the log adapter should do this automatically after recovery; (2) if we make KeyValueStoreMemory guarantee immediate reads, we should be able to get rid of the discardCommit() below and not need a writable log adapter

	TraceEvent("RTSSComplete", self->dbgid);

	return Void();
}

ACTOR Future<Void> sendInitialCommitToResolvers( Reference<MasterData> self ) {
	state KeyRange txnKeys = allKeys;
	state Sequence txnSequence = 0;
	ASSERT(self->recoveryTransactionVersion);

	state Standalone<VectorRef<KeyValueRef>> data = self->txnStateStore->readRange(txnKeys, BUGGIFY ? 3 : SERVER_KNOBS->DESIRED_TOTAL_BYTES, SERVER_KNOBS->DESIRED_TOTAL_BYTES).get();
	state vector<Future<Void>> txnReplies;
	state int64_t dataOutstanding = 0;
	loop {
		if(!data.size()) break;
		((KeyRangeRef&)txnKeys) = KeyRangeRef( keyAfter(data.back().key, txnKeys.arena()), txnKeys.end );
		Standalone<VectorRef<KeyValueRef>> nextData = self->txnStateStore->readRange(txnKeys, BUGGIFY ? 3 : SERVER_KNOBS->DESIRED_TOTAL_BYTES, SERVER_KNOBS->DESIRED_TOTAL_BYTES).get();

		for(auto& r : self->proxies) {
			TxnStateRequest req;
			req.arena = data.arena();
			req.data = data;
			req.sequence = txnSequence;
			req.last = !nextData.size();
			txnReplies.push_back( brokenPromiseToNever( r.txnState.getReply( req ) ) );
			dataOutstanding += data.arena().getSize();
		}
		data = nextData;
		txnSequence++;

		if(dataOutstanding > SERVER_KNOBS->MAX_TXS_SEND_MEMORY) {
			Void _ = wait( waitForAll(txnReplies) );
			txnReplies = vector<Future<Void>>();
			dataOutstanding = 0;
		}

		Void _ = wait(yield());
	}
	Void _ = wait( waitForAll(txnReplies) );

	vector<Future<ResolveTransactionBatchReply>> replies;
	for(auto& r : self->resolvers) {
		ResolveTransactionBatchRequest req;
		req.prevVersion = -1;
		req.version = self->lastEpochEnd;
		req.lastReceivedVersion = -1;

		replies.push_back( brokenPromiseToNever( r.resolve.getReply( req ) ) );
	}

	Void _ = wait(waitForAll(replies));
	return Void();
}

ACTOR Future<Void> triggerUpdates( Reference<MasterData> self, Reference<ILogSystem> oldLogSystem ) {
	loop {
		Void _ = wait( oldLogSystem->onLogSystemConfigChange() || self->cstate.fullyRecovered.getFuture() );
		if(self->cstate.fullyRecovered.isSet())
			return Void();

		self->registrationTrigger.trigger();
	}
}

ACTOR Future<Void> discardCommit(IKeyValueStore* store, LogSystemDiskQueueAdapter* adapter) {
	state Future<LogSystemDiskQueueAdapter::CommitMessage> fcm = adapter->getCommitMessage();
	state Future<Void> committed = store->commit();
	LogSystemDiskQueueAdapter::CommitMessage cm = wait(fcm);
	ASSERT(!committed.isReady());
	cm.acknowledge.send(Void());
	ASSERT(committed.isReady());
	return Void();
}

ACTOR Future<Void> recoverFrom( Reference<MasterData> self, Reference<ILogSystem> oldLogSystem, vector<StorageServerInterface>* seedServers, vector<Standalone<CommitTransactionRef>>* initialConfChanges ) {
	TraceEvent("MasterRecoveryState", self->dbgid)
		.detail("StatusCode", RecoveryStatus::reading_transaction_system_state)
		.detail("Status", RecoveryStatus::names[RecoveryStatus::reading_transaction_system_state])
		.trackLatest("MasterRecoveryState");
	self->hasConfiguration = false;

	if(BUGGIFY)
		Void _ = wait( delay(10.0) );

	Void _ = wait( readTransactionSystemState( self, oldLogSystem ) );
	for (auto& itr : *initialConfChanges) {
		for(auto& m : itr.mutations) {
			self->configuration.applyMutation( m );
		}
	}

	debug_checkMaxRestoredVersion( UID(), self->lastEpochEnd, "DBRecovery" );

	// Ordinarily we pass through this loop once and recover.  We go around the loop if recovery stalls for more than a second,
	// a provisional master is initialized, and an "emergency transaction" is submitted that might change the configuration so that we can
	// finish recovery.
	state Future<Void> recruitments = recruitEverything( self, seedServers, oldLogSystem, initialConfChanges );
	loop {
		state Future<Standalone<CommitTransactionRef>> provisional = provisionalMaster(self, delay(1.0));

		choose {
			when (Void _ = wait( recruitments )) {
				provisional.cancel();
				break;
			}
			when (Standalone<CommitTransactionRef> _req = wait( provisional )) {
				state Standalone<CommitTransactionRef> req = _req;  // mutable
				TEST(true);  // Emergency transaction processing during recovery
				TraceEvent("EmergencyTransaction", self->dbgid);
				for (auto m = req.mutations.begin(); m != req.mutations.end(); ++m)
					TraceEvent("EmergencyTransactionMutation", self->dbgid).detail("MType", m->type).detail("P1", printable(m->param1)).detail("P2", printable(m->param2));

				DatabaseConfiguration oldConf = self->configuration;
				self->configuration = self->originalConfiguration;
				for(auto& m : req.mutations)
					self->configuration.applyMutation( m );

				initialConfChanges->clear();
				initialConfChanges->push_back(req);

				if(self->configuration != oldConf) { //confChange does not trigger when including servers
					recruitments = recruitEverything( self, seedServers, oldLogSystem, initialConfChanges );
				}
			}
		}

		provisional.cancel();
	}

	return Void();
}

ACTOR Future<Void> getVersion(Reference<MasterData> self, GetCommitVersionRequest req) {
	state std::map<UID, ProxyVersionReplies>::iterator proxyItr = self->lastProxyVersionReplies.find(req.requestingProxy); // lastProxyVersionReplies never changes

	if (proxyItr == self->lastProxyVersionReplies.end()) {
		// Request from invalid proxy (e.g. from duplicate recruitment request)
		req.reply.send(Never());
		return Void();
	}

	TEST(proxyItr->second.latestRequestNum.get() < req.requestNum - 1); // Commit version request queued up
	Void _ = wait(proxyItr->second.latestRequestNum.whenAtLeast(req.requestNum-1));

	auto itr = proxyItr->second.replies.find(req.requestNum);
	if (itr != proxyItr->second.replies.end()) {
		TEST(true); // Duplicate request for sequence
		req.reply.send(itr->second);
	}
	else if(req.requestNum <= proxyItr->second.latestRequestNum.get()) {
		TEST(true); // Old request for previously acknowledged sequence - may be impossible with current FlowTransport implementation
		ASSERT( req.requestNum < proxyItr->second.latestRequestNum.get() );  // The latest request can never be acknowledged
		req.reply.send(Never());
	}
	else {
		GetCommitVersionReply rep;

		if(self->version == invalidVersion) {
			self->lastVersionTime = now();
			self->version = self->recoveryTransactionVersion;
			rep.prevVersion = self->lastEpochEnd;
		}
		else {
			double t1 = now();
			if(BUGGIFY) {
				t1 = self->lastVersionTime;
			}
			rep.prevVersion = self->version;
			self->version += std::max(1, std::min(SERVER_KNOBS->MAX_READ_TRANSACTION_LIFE_VERSIONS, int(SERVER_KNOBS->VERSIONS_PER_SECOND*(t1-self->lastVersionTime))));

			TEST( self->version - rep.prevVersion == 1 );  // Minimum possible version gap
			TEST( self->version - rep.prevVersion == SERVER_KNOBS->MAX_READ_TRANSACTION_LIFE_VERSIONS );  // Maximum possible version gap
			self->lastVersionTime = t1;

			if(self->resolverNeedingChanges.count(req.requestingProxy)) {
				rep.resolverChanges = self->resolverChanges.get();
				rep.resolverChangesVersion = self->resolverChangesVersion;
				self->resolverNeedingChanges.erase(req.requestingProxy);

				if(self->resolverNeedingChanges.empty())
					self->resolverChanges.set(Standalone<VectorRef<ResolverMoveRef>>());
			}
		}

		rep.version = self->version;
		rep.requestNum = req.requestNum;

		proxyItr->second.replies.erase(proxyItr->second.replies.begin(), proxyItr->second.replies.upper_bound(req.mostRecentProcessedRequestNum));
		proxyItr->second.replies[req.requestNum] = rep;
		ASSERT(rep.prevVersion >= 0);
		req.reply.send(rep);

		ASSERT(proxyItr->second.latestRequestNum.get() == req.requestNum - 1);
		proxyItr->second.latestRequestNum.set(req.requestNum);
	}

	return Void();
}

ACTOR Future<Void> provideVersions(Reference<MasterData> self) {
	state ActorCollection versionActors(false);

	for (auto& p : self->proxies)
		self->lastProxyVersionReplies[p.id()] = ProxyVersionReplies();

	loop {
		choose {
			when(GetCommitVersionRequest req = waitNext(self->myInterface.getCommitVersion.getFuture())) {
				versionActors.add(getVersion(self, req));
			}
			when(Void _ = wait(versionActors.getResult())) { }
		}
	}
}

std::pair<KeyRangeRef, bool> findRange( CoalescedKeyRangeMap<int>& key_resolver, Standalone<VectorRef<ResolverMoveRef>>& movedRanges, int src, int dest ) {
	auto ranges = key_resolver.ranges();
	auto prev = ranges.begin();
	auto it = ranges.begin();
	++it;
	if(it==ranges.end()) {
		if(ranges.begin().value() != src || std::find(movedRanges.begin(), movedRanges.end(), ResolverMoveRef(ranges.begin()->range(), dest)) != movedRanges.end())
			throw operation_failed();
		return std::make_pair(ranges.begin().range(), true);
	}

	std::set<int> borders;
	//If possible expand an existing boundary between the two resolvers
	for(; it != ranges.end(); ++it) {
		if(it->value() == src && prev->value() == dest && std::find(movedRanges.begin(), movedRanges.end(), ResolverMoveRef(it->range(), dest)) == movedRanges.end()) {
			return std::make_pair(it->range(), true);
		}
		if(it->value() == dest && prev->value() == src && std::find(movedRanges.begin(), movedRanges.end(), ResolverMoveRef(prev->range(), dest)) == movedRanges.end()) {
			return std::make_pair(prev->range(), false);
		}
		if(it->value() == dest)
			borders.insert(prev->value());
		if(prev->value() == dest)
			borders.insert(it->value());
		++prev;
	}

	prev = ranges.begin();
	it = ranges.begin();
	++it;
	//If possible create a new boundry which doesn't exist yet
	for(; it != ranges.end(); ++it) {
		if(it->value() == src && !borders.count(prev->value()) && std::find(movedRanges.begin(), movedRanges.end(), ResolverMoveRef(it->range(), dest)) == movedRanges.end()) {
			return std::make_pair(it->range(), true);
		}
		if(prev->value() == src && !borders.count(it->value()) && std::find(movedRanges.begin(), movedRanges.end(), ResolverMoveRef(prev->range(), dest)) == movedRanges.end()) {
			return std::make_pair(prev->range(), false);
		}
		++prev;
	}

	it = ranges.begin();
	for(; it != ranges.end(); ++it) {
		if(it->value() == src && std::find(movedRanges.begin(), movedRanges.end(), ResolverMoveRef(it->range(), dest)) == movedRanges.end()) {
			return std::make_pair(it->range(), true);
		}
	}
	throw operation_failed(); //we are already attempting to move all of the data one resolver is assigned, so do not move anything
}

ACTOR Future<Void> resolutionBalancing(Reference<MasterData> self) {
	state CoalescedKeyRangeMap<int> key_resolver;
	key_resolver.insert(allKeys, 0);
	loop {
		Void _ = wait(delay(SERVER_KNOBS->MIN_BALANCE_TIME, TaskResolutionMetrics));
		while(self->resolverChanges.get().size())
			Void _ = wait(self->resolverChanges.onChange());
		state std::vector<Future<int64_t>> futures;
		for (auto& p : self->resolvers)
			futures.push_back(brokenPromiseToNever(p.metrics.getReply(ResolutionMetricsRequest(), TaskResolutionMetrics)));
		Void _ = wait( waitForAll(futures) );
		state IndexedSet<std::pair<int64_t, int>, NoMetric> metrics;

		int64_t total = 0;
		for (int i = 0; i < futures.size(); i++) {
			total += futures[i].get();
			metrics.insert(std::make_pair(futures[i].get(), i), NoMetric());
			//TraceEvent("ResolverMetric").detail("I", i).detail("Metric", futures[i].get());
		}
		if( metrics.lastItem()->first - metrics.begin()->first > SERVER_KNOBS->MIN_BALANCE_DIFFERENCE ) {
			try {
				state int src = metrics.lastItem()->second;
				state int dest = metrics.begin()->second;
				state int64_t amount = std::min( metrics.lastItem()->first - total/self->resolvers.size(), total/self->resolvers.size() - metrics.begin()->first ) / 2;
				state Standalone<VectorRef<ResolverMoveRef>> movedRanges;

				loop {
					state std::pair<KeyRangeRef, bool> range = findRange( key_resolver, movedRanges, src, dest );

					ResolutionSplitRequest req;
					req.front = range.second;
					req.offset = amount;
					req.range = range.first;

					ResolutionSplitReply split = wait( brokenPromiseToNever(self->resolvers[metrics.lastItem()->second].split.getReply(req, TaskResolutionMetrics)) );
					KeyRangeRef moveRange = range.second ? KeyRangeRef( range.first.begin, split.key ) : KeyRangeRef( split.key, range.first.end );
					movedRanges.push_back_deep(movedRanges.arena(), ResolverMoveRef(moveRange, dest));
					TraceEvent("MovingResolutionRange").detail("Src", src).detail("Dest", dest).detail("Amount", amount).detail("StartRange", printable(range.first)).detail("MoveRange", printable(moveRange)).detail("Used", split.used).detail("KeyResolverRanges", key_resolver.size());
					amount -= split.used;
					if(moveRange != range.first || amount <= 0 )
						break;
				}
				for(auto& it : movedRanges)
					key_resolver.insert(it.range, it.dest);
				//for(auto& it : key_resolver.ranges())
				//	TraceEvent("KeyResolver").detail("Range", printable(it.range())).detail("Value", it.value());

				self->resolverChangesVersion = self->version + 1;
				for (auto& p : self->proxies)
					self->resolverNeedingChanges.insert(p.id());
				self->resolverChanges.set(movedRanges);
			} catch( Error&e ) {
				if(e.code() != error_code_operation_failed)
					throw;
			}
		}
	}
}

static std::set<int> const& normalMasterErrors() {
	static std::set<int> s;
	if (s.empty()) {
		s.insert( error_code_tlog_stopped );
		s.insert( error_code_master_tlog_failed );
		s.insert( error_code_master_proxy_failed );
		s.insert( error_code_master_resolver_failed );
		s.insert( error_code_recruitment_failed );
		s.insert( error_code_no_more_servers );
		s.insert( error_code_master_recovery_failed );
		s.insert( error_code_coordinated_state_conflict );
		s.insert( error_code_movekeys_conflict );
		s.insert( error_code_master_max_versions_in_flight );
		s.insert( error_code_worker_removed );
		s.insert( error_code_new_coordinators_timed_out );
		s.insert( error_code_broken_promise );
	}
	return s;
}

ACTOR Future<Void> changeCoordinators( Reference<MasterData> self ) {
	loop {
		ChangeCoordinatorsRequest req = waitNext( self->myInterface.changeCoordinators.getFuture() );
		state ChangeCoordinatorsRequest changeCoordinatorsRequest = req;

		while( !self->cstate.previousWrite.isReady() ) {
			Void _ = wait( self->cstate.previousWrite );
			Void _ = wait( delay(0) ); //if a new core state is ready to be written, have that take priority over our finalizing write;
		}

		if(!self->cstate.fullyRecovered.isSet()) {
			Void _ = wait( self->cstate.write(self->cstate.myDBState, true) );
		}

		try {
			Void _ = wait( self->cstate.move( ClusterConnectionString( changeCoordinatorsRequest.newConnectionString.toString() ) ) );
		}
		catch(Error &e) {
			if(e.code() != error_code_actor_cancelled)
				changeCoordinatorsRequest.reply.sendError(e);

			throw;
		}

		throw internal_error();
	}
}

ACTOR Future<Void> rejoinRequestHandler( Reference<MasterData> self ) {
	loop {
		TLogRejoinRequest req = waitNext( self->myInterface.tlogRejoin.getFuture() );
		req.reply.send(true);
	}
}

ACTOR Future<Void> trackTlogRecovery( Reference<MasterData> self, Reference<AsyncVar<Reference<ILogSystem>>> oldLogSystems, Promise<Void> remoteRecovered ) {
	state Future<Void> rejoinRequests = Never();
	state DBRecoveryCount recoverCount = self->cstate.myDBState.recoveryCount + 1;
	loop {
		state DBCoreState newState;
		self->logSystem->toCoreState( newState );
		newState.recoveryCount = recoverCount;
		state Future<Void> changed = self->logSystem->onCoreStateChanged();
		ASSERT( newState.tLogs[0].tLogWriteAntiQuorum == self->configuration.tLogWriteAntiQuorum && newState.tLogs[0].tLogReplicationFactor == self->configuration.tLogReplicationFactor );

		state bool allLogs = newState.tLogs.size() == self->configuration.expectedLogSets(self->primaryDcId.size() ? self->primaryDcId[0] : Optional<Key>());
		state bool finalUpdate = !newState.oldTLogData.size() && allLogs;
		Void _ = wait( self->cstate.write(newState, finalUpdate) );
		self->logSystem->coreStateWritten(newState);
		if(self->cstateUpdated.canBeSet()) {
			self->cstateUpdated.send(Void());
		}
		
		if( finalUpdate ) {
			self->recoveryState = RecoveryState::REMOTE_RECOVERED;
			TraceEvent("MasterRecoveryState", self->dbgid)
			.detail("StatusCode", RecoveryStatus::remote_recovered)
			.detail("Status", RecoveryStatus::names[RecoveryStatus::remote_recovered])
			.trackLatest(format("%s/MasterRecoveryState", printable(self->dbName).c_str() ).c_str());
		}
		
		self->registrationTrigger.trigger();

		if(allLogs && remoteRecovered.canBeSet()) {
			remoteRecovered.send(Void());
		}
		
		if( finalUpdate ) {
			oldLogSystems->get()->stopRejoins();
			rejoinRequests = rejoinRequestHandler(self);
			return Void();
		}

		Void _ = wait( changed );
	}
}

ACTOR Future<Void> configurationMonitor( Reference<MasterData> self ) {
	state Database cx = openDBOnServer(self->dbInfo, TaskDefaultEndpoint, true, true);
	loop {
		state ReadYourWritesTransaction tr(cx);

		loop {
			try {
				tr.setOption(FDBTransactionOptions::ACCESS_SYSTEM_KEYS);
				Standalone<RangeResultRef> results = wait( tr.getRange( configKeys, CLIENT_KNOBS->TOO_MANY ) ); 
				ASSERT( !results.more && results.size() < CLIENT_KNOBS->TOO_MANY );

				DatabaseConfiguration conf;
				conf.fromKeyValues((VectorRef<KeyValueRef>) results);
				if(conf != self->configuration) {
					self->configuration = conf;
					self->registrationTrigger.trigger();
				}

				state Future<Void> watchFuture = tr.watch(excludedServersVersionKey);
				Void _ = wait(tr.commit());
				Void _ = wait(watchFuture);
				break;
			} catch (Error& e) {
				Void _ = wait( tr.onError(e) );
			}
		}
	}
}

ACTOR Future<Void> masterCore( Reference<MasterData> self ) {
	state TraceInterval recoveryInterval("MasterRecovery");
	state double recoverStartTime = now();

	self->addActor.send( waitFailureServer(self->myInterface.waitFailure.getFuture()) );

	TraceEvent( recoveryInterval.begin(), self->dbgid );

	self->recoveryState = RecoveryState::READING_CSTATE;
	TraceEvent("MasterRecoveryState", self->dbgid)
		.detail("StatusCode", RecoveryStatus::reading_coordinated_state)
		.detail("Status", RecoveryStatus::names[RecoveryStatus::reading_coordinated_state])
		.trackLatest("MasterRecoveryState");

	Void _ = wait( self->cstate.read() );

	self->recoveryState = RecoveryState::LOCKING_CSTATE;
	TraceEvent("MasterRecoveryState", self->dbgid)
		.detail("StatusCode", RecoveryStatus::locking_coordinated_state)
		.detail("Status", RecoveryStatus::names[RecoveryStatus::locking_coordinated_state])
		.detail("TLogs", self->cstate.prevDBState.tLogs.size())
		.detail("MyRecoveryCount", self->cstate.prevDBState.recoveryCount+2)
		.trackLatest("MasterRecoveryState");

	state Reference<AsyncVar<Reference<ILogSystem>>> oldLogSystems( new AsyncVar<Reference<ILogSystem>> );
	state Future<Void> recoverAndEndEpoch = ILogSystem::recoverAndEndEpoch(oldLogSystems, self->dbgid, self->cstate.prevDBState, self->myInterface.tlogRejoin.getFuture(), self->myInterface.locality);

	DBCoreState newState = self->cstate.myDBState;
	newState.recoveryCount++;
	Void _ = wait( self->cstate.write(newState) || recoverAndEndEpoch );

	self->recoveryState = RecoveryState::RECRUITING;

	state vector<StorageServerInterface> seedServers;
	state vector<Standalone<CommitTransactionRef>> initialConfChanges;
	state Future<Void> logChanges;

	loop {
		Reference<ILogSystem> oldLogSystem = oldLogSystems->get();
		if(oldLogSystem) logChanges = triggerUpdates(self, oldLogSystem);

		state Future<Void> reg = oldLogSystem ? updateRegistration(self, oldLogSystem) : Never();
		self->registrationTrigger.trigger();

		choose {
			when (Void _ = wait( oldLogSystem ? recoverFrom(self, oldLogSystem, &seedServers, &initialConfChanges) : Never() )) { reg.cancel(); break; }
			when (Void _ = wait( oldLogSystems->onChange() )) {}
			when (Void _ = wait( reg )) { throw internal_error(); }
			when (Void _ = wait( recoverAndEndEpoch )) {}
		}
	}

	recoverAndEndEpoch.cancel();

	ASSERT( self->proxies.size() <= self->configuration.getDesiredProxies() );
	ASSERT( self->resolvers.size() <= self->configuration.getDesiredResolvers() );

	self->recoveryState = RecoveryState::RECOVERY_TRANSACTION;
	TraceEvent("MasterRecoveryState", self->dbgid)
		.detail("StatusCode", RecoveryStatus::recovery_transaction)
		.detail("Status", RecoveryStatus::names[RecoveryStatus::recovery_transaction])
		.trackLatest("MasterRecoveryState");

	// Recovery transaction
	state bool debugResult = debug_checkMinRestoredVersion( UID(), self->lastEpochEnd, "DBRecovery", SevWarn );

	CommitTransactionRequest recoveryCommitRequest;
	recoveryCommitRequest.flags = recoveryCommitRequest.flags | CommitTransactionRequest::FLAG_IS_LOCK_AWARE;
	CommitTransactionRef &tr = recoveryCommitRequest.transaction;
	int mmApplied = 0;  // The number of mutations in tr.mutations that have been applied to the txnStateStore so far
	if (self->lastEpochEnd != 0) {
		// This transaction sets \xff/lastEpochEnd, which the shard servers can use to roll back speculatively
		//   processed semi-committed transactions from the previous epoch.
		// It also guarantees the shard servers and tlog servers eventually get versions in the new epoch, which
		//   clients might rely on.
		// This transaction is by itself in a batch (has its own version number), which simplifies storage servers slightly (they assume there are no modifications to serverKeys in the same batch)
		// The proxy also expects the lastEpochEndKey mutation to be first in the transaction
		BinaryWriter bw(Unversioned());
		tr.set(recoveryCommitRequest.arena, lastEpochEndKey, (bw << self->lastEpochEnd).toStringRef());
	} else {
		// Recruit and seed initial shard servers
		// This transaction must be the very first one in the database (version 1)
		seedShardServers(recoveryCommitRequest.arena, tr, seedServers);
	}
	// initialConfChanges have not been conflict checked against any earlier writes in the recovery transaction, so do this as early as possible in the recovery transaction
	// but see above comments as to why it can't be absolutely first.  Theoretically emergency transactions should conflict check against the lastEpochEndKey.
	for (auto& itr : initialConfChanges) {
		tr.mutations.append_deep(recoveryCommitRequest.arena, itr.mutations.begin(), itr.mutations.size());
		tr.write_conflict_ranges.append_deep(recoveryCommitRequest.arena, itr.write_conflict_ranges.begin(), itr.write_conflict_ranges.size());
	}

	tr.set(recoveryCommitRequest.arena, backupVersionKey, backupVersionValue);
	tr.set(recoveryCommitRequest.arena, coordinatorsKey, self->coordinators.ccf->getConnectionString().toString());
	tr.set(recoveryCommitRequest.arena, logsKey, self->logSystem->getLogsValue());

	applyMetadataMutations(self->dbgid, recoveryCommitRequest.arena, tr.mutations.slice(mmApplied, tr.mutations.size()), self->txnStateStore, NULL, NULL);
	mmApplied = tr.mutations.size();

	tr.read_snapshot = self->recoveryTransactionVersion;  // lastEpochEnd would make more sense, but isn't in the initial window of the resolver(s)

	TraceEvent("MasterRecoveryCommit", self->dbgid);
	state Future<ErrorOr<CommitID>> recoveryCommit = self->proxies[0].commit.tryGetReply(recoveryCommitRequest);
	self->addActor.send( self->logSystem->onError() );
	self->addActor.send( waitResolverFailure( self->resolvers ) );
	self->addActor.send( waitProxyFailure( self->proxies ) );
	self->addActor.send( provideVersions(self) );
	self->addActor.send( reportErrors(updateRegistration(self, self->logSystem), "UpdateRegistration", self->dbgid) );
	self->registrationTrigger.trigger();

	Void _ = wait(discardCommit(self->txnStateStore, self->txnStateLogAdapter));

	// Wait for the recovery transaction to complete.
	// SOMEDAY: For faster recovery, do this and setDBState asynchronously and don't wait for them
	// unless we want to change TLogs
	Void _ = wait((success(recoveryCommit) && sendInitialCommitToResolvers(self)) );
	if(recoveryCommit.isReady() && recoveryCommit.get().isError()) {
		TEST(true);  // Master recovery failed because of the initial commit failed
		throw master_recovery_failed();
	}

	ASSERT( self->recoveryTransactionVersion != 0 );

	self->recoveryState = RecoveryState::WRITING_CSTATE;
	TraceEvent("MasterRecoveryState", self->dbgid)
		.detail("StatusCode", RecoveryStatus::writing_coordinated_state)
		.detail("Status", RecoveryStatus::names[RecoveryStatus::writing_coordinated_state])
		.detail("TLogList", self->logSystem->describe())
		.trackLatest("MasterRecoveryState");

	// Multiple masters prevent conflicts between themselves via CoordinatedState (self->cstate)
	//  1. If SetMaster succeeds, then by CS's contract, these "new" Tlogs are the immediate
	//     successors of the "old" ones we are replacing
	//  2. logSystem->recoverAndEndEpoch ensured that a co-quorum of the "old" tLogs were stopped at
	//     versions <= self->lastEpochEnd, so no versions > self->lastEpochEnd could be (fully) committed to them.
	//  3. No other master will attempt to commit anything to our "new" Tlogs
	//     because it didn't recruit them
	//  4. Therefore, no full commit can come between self->lastEpochEnd and the first commit
	//     we made to the new Tlogs (self->recoveryTransactionVersion), and only our own semi-commits can come between our
	//     first commit and the next new TLogs

	state Promise<Void> remoteRecovered;
	self->addActor.send( trackTlogRecovery(self, oldLogSystems, remoteRecovered) );
	debug_advanceMaxCommittedVersion(UID(), self->recoveryTransactionVersion);
	Void _ = wait(self->cstateUpdated.getFuture());
	debug_advanceMinCommittedVersion(UID(), self->recoveryTransactionVersion);

	if( debugResult )
		TraceEvent(SevError, "DBRecoveryDurabilityError");

	TraceEvent("MasterCommittedTLogs", self->dbgid).detail("TLogs", self->logSystem->describe()).detail("RecoveryCount", self->cstate.myDBState.recoveryCount).detail("RecoveryTransactionVersion", self->recoveryTransactionVersion);

	TraceEvent(recoveryInterval.end(), self->dbgid).detail("RecoveryTransactionVersion", self->recoveryTransactionVersion);

	self->recoveryState = RecoveryState::FULLY_RECOVERED;
	double recoveryDuration = now() - recoverStartTime;

	TraceEvent((recoveryDuration > 4 && !g_network->isSimulated()) ? SevWarnAlways : SevInfo, "MasterRecoveryDuration", self->dbgid)
		.detail("RecoveryDuration", recoveryDuration)
		.trackLatest("MasterRecoveryDuration");

	TraceEvent("MasterRecoveryState", self->dbgid)
		.detail("StatusCode", RecoveryStatus::fully_recovered)
		.detail("Status", RecoveryStatus::names[RecoveryStatus::fully_recovered])
		.detail("StoreType", self->configuration.storageServerStoreType)
		.detail("RecoveryDuration", recoveryDuration)
		.trackLatest("MasterRecoveryState");

	// Now that the master is recovered we can start auxiliary services that happen to run here
	{
		PromiseStream< std::pair<UID, Optional<StorageServerInterface>> > ddStorageServerChanges;
		state double lastLimited = 0;
		self->addActor.send( reportErrorsExcept( dataDistribution( self->dbInfo, self->myInterface, self->configuration, ddStorageServerChanges, self->logSystem, self->recoveryTransactionVersion, self->primaryDcId, self->remoteDcIds, &lastLimited, remoteRecovered.getFuture() ), "DataDistribution", self->dbgid, &normalMasterErrors() ) );
		self->addActor.send( reportErrors( rateKeeper( self->dbInfo, ddStorageServerChanges, self->myInterface.getRateInfo.getFuture(), self->dbName, self->configuration, &lastLimited ), "Ratekeeper", self->dbgid) );
	}

	if( self->resolvers.size() > 1 )
		self->addActor.send( resolutionBalancing(self) );

	self->addActor.send( changeCoordinators(self) );
	self->addActor.send( configurationMonitor( self ) );

	Void _ = wait( Future<Void>(Never()) );
	throw internal_error();
}

ACTOR Future<Void> masterServer( MasterInterface mi, Reference<AsyncVar<ServerDBInfo>> db, ServerCoordinators coordinators, LifetimeToken lifetime )
{
	state Future<Void> onDBChange = Void();
	state PromiseStream<Future<Void>> addActor;
	state Reference<MasterData> self( new MasterData( db, mi, coordinators, db->get().clusterInterface, db->get().dbName, LiteralStringRef(""), addActor ) );
	state Future<Void> collection = actorCollection( self->addActor.getFuture() );

	TEST( !lifetime.isStillValid( db->get().masterLifetime, mi.id()==db->get().master.id() ) );  // Master born doomed
	TraceEvent("MasterLifetime", self->dbgid).detail("LifetimeToken", lifetime.toString());

	try {
		state Future<Void> core = masterCore( self );
		loop choose {
			when (Void _ = wait( core )) { break; }
			when (Void _ = wait( onDBChange )) {
				onDBChange = db->onChange();
				if (!lifetime.isStillValid( db->get().masterLifetime, mi.id()==db->get().master.id() )) {
					TraceEvent("MasterTerminated", mi.id()).detail("Reason", "LifetimeToken").detail("MyToken", lifetime.toString()).detail("CurrentToken", db->get().masterLifetime.toString());
					TEST(true);  // Master replaced, dying
					if (BUGGIFY) Void _ = wait( delay(5) );
					throw worker_removed();
				}
			}
			when (Void _ = wait(collection) ) { ASSERT(false); throw internal_error(); }
		}
	} catch (Error& e) {
		state Error err = e;
		if(e.code() != error_code_actor_cancelled) {
			Void _ = wait(delay(0.0));
		}

		while(!self->addActor.isEmpty()) {
			self->addActor.getFuture().pop();
		}
			
		TEST(err.code() == error_code_master_tlog_failed);  // Master: terminated because of a tLog failure
		TEST(err.code() == error_code_master_proxy_failed);  // Master: terminated because of a proxy failure
		TEST(err.code() == error_code_master_resolver_failed);  // Master: terminated because of a resolver failure

		if (normalMasterErrors().count(err.code()))
		{
			TraceEvent("MasterTerminated", mi.id()).error(err);
			return Void();
		}
		throw err;
	}
	return Void();
}
