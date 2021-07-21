/* 
 * Copyright (c) 2020, Daniel Beßler
 * All rights reserved.
 * 
 * This file is part of KnowRob, please consult
 * https://github.com/knowrob/knowrob for license details.
 */

#include "knowrob/db/mongo/MongoWatch.h"
#include "knowrob/db/mongo/MongoException.h"
#include "knowrob/db/mongo/MongoInterface.h"
#include "knowrob/db/mongo/bson_pl.h"

#include <sstream>
#include <iostream>

// SWI Prolog
#define PL_SAFE_ARG_MACROS
#include <SWI-cpp.h>

// TODO: make configurable
#define WATCH_RATE_MS 200

MongoWatch::MongoWatch(mongoc_client_pool_t *client_pool)
: client_pool_(client_pool),
  isRunning_(false),
  thread_(NULL),
  id_counter_(0)
{
	// TODO: consider using one thread per collection,
	//       _and_ one stream per collection.
	//       then build a composite $match covering all
	//       watch requests.
	//       finally do some processing client side to decide
	//       which callback must be notified.
	// TODO: special handling for remove events as they do not
	//       provide the values $match cannot be used
}

MongoWatch::~MongoWatch()
{
	// first let the thread terminate
	stopWatchThread();
	// then stop all watcher
	std::lock_guard<std::mutex> guard(lock_);
	for(std::map<long, MongoWatcher*>::iterator
			it=watcher_map_.begin(); it!=watcher_map_.end(); ++it)
	{
		MongoWatcher *watcher = it->second;
		delete watcher;
	}
	watcher_map_.clear();
}

void MongoWatch::startWatchThread()
{
	std::lock_guard<std::mutex> guard(lock_);
	if(!isRunning_ && !thread_) {
		isRunning_ = true;
		thread_ = new std::thread(&MongoWatch::loop, this);
	}
}

void MongoWatch::stopWatchThread()
{
	std::lock_guard<std::mutex> guard(lock_);
	if(isRunning_ && thread_) {
		isRunning_ = false;
		thread_->join();
		delete thread_;
		thread_ = NULL;
	}
}

long MongoWatch::watch(
		const char *db_name,
		const char *coll_name,
		const std::string &callback_goal,
		const PlTerm &query_term)
{
	// add to map
	{
		MongoWatcher *watcher = new MongoWatcher(client_pool_,
				db_name, coll_name, callback_goal, query_term);
		std::lock_guard<std::mutex> guard(lock_);
		watcher_map_[id_counter_] = watcher;
		id_counter_ += 1;
	}
	// start the thread when the first watch is added
	startWatchThread();
	return id_counter_-1;
}

void MongoWatch::unwatch(long watcher_id)
{
	std::map<long, MongoWatcher*>::iterator
		needle = watcher_map_.find(watcher_id);
	if(needle != watcher_map_.end()) {
		MongoWatcher *watcher = needle->second;
		std::lock_guard<std::mutex> guard(lock_);
		watcher_map_.erase(needle);
		delete watcher;
	}
	if(watcher_map_.empty()) {
		stopWatchThread();
	}
}

void MongoWatch::loop()
{
	// bind a Prolog engine to this thread.
	// this is needed as callback's are predicates in the
	// Prolog knowledge base.
	if(!PL_thread_attach_engine(NULL)) {
		std::cerr << "failed to attach engine!" << std::endl;
		isRunning_ = false;
	}
	// loop as long isRunning_=true
	auto next = std::chrono::system_clock::now();
	while(isRunning_) {
		{
			std::lock_guard<std::mutex> guard(lock_);
			for(std::map<long, MongoWatcher*>::iterator
					it=watcher_map_.begin(); it!=watcher_map_.end(); ++it)
			{
				it->second->next(it->first);
			}
		}
		// try to run at constant rate
		next += std::chrono::milliseconds(WATCH_RATE_MS);
		std::this_thread::sleep_until(next);
	}
}


MongoWatcher::MongoWatcher(
		mongoc_client_pool_t *pool,
		const char *db_name,
		const char *coll_name,
		const std::string &callback_goal,
		const PlTerm &query_term)
: collection_(NULL),
  stream_(NULL),
  callback_goal_(callback_goal)
{
	// create pipeline object
	bson_t *pipeline = bson_new();
	bson_error_t err;
	if(!bsonpl_concat(pipeline, query_term, &err)) {
		bson_destroy(pipeline);
		throw MongoException("invalid_query",err);
	}
	// create options object
	bson_t *opts = BCON_NEW(
		// the watcher should be non-blocking
		// TODO: why zero is not allowed?
		"maxAwaitTimeMS", BCON_INT32(1),
		// always fetch full document
		// TODO: allow to configure this
		"fullDocument",   BCON_UTF8("updateLookup")
		//"batchSize", ..
	);
	// connect and append session ID to options
	collection_ = new MongoCollection(pool, db_name, coll_name);
	collection_->appendSession(opts);
	// create the stream object
	stream_ = mongoc_collection_watch(
			(*collection_)(), pipeline, opts);
	// cleanup
	bson_destroy(pipeline);
	bson_destroy(opts);
}

MongoWatcher::~MongoWatcher()
{
	if(stream_!=NULL) {
		mongoc_change_stream_destroy(stream_);
		stream_ = NULL;
	}
	if(collection_!=NULL) {
		delete collection_;
		collection_ = NULL;
	}
}

bool MongoWatcher::next(long watcher_id)
{
	if(stream_==NULL) {
		// stream had an error before
		return false;
	}
	// try retrieving next document
	const bson_t *doc;
	if(mongoc_change_stream_next(stream_, &doc)) {
		PlTerm term = bson_to_term(doc);
		PlCall(callback_goal_.c_str(), PlTermv(PlTerm((long)watcher_id), term));
		return true;
	}
	else {
		// check if stream has an error
		const bson_t *err_doc;
		bson_error_t error;
		if(mongoc_change_stream_error_document(stream_, &error, &err_doc)) {
			if(!bson_empty (err_doc)) {
				fprintf(stderr, "mongodb server error: %s\n", bson_as_relaxed_extended_json (err_doc, NULL));
			}
			else {
				fprintf (stderr, "mongodb client error: %s\n", error.message);
			}
			mongoc_change_stream_destroy(stream_);
			stream_ = NULL;
			return false;
		}
		else {
			return true;
		}
	}
}
