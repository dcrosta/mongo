/**
 * Copyright (c) 2011 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include "pch.h"

#include <boost/unordered_map.hpp>
#include "util/intrusive_counter.h"
#include "db/clientcursor.h"
#include "db/jsobj.h"
#include "db/pipeline/dependency_tracker.h"
#include "db/pipeline/document.h"
#include "db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "db/pipeline/value.h"
#include "util/string_writer.h"
#include "mongo/db/projection.h"

namespace mongo {
    class Accumulator;
    class Cursor;
    class DependencyTracker;
    class Document;
    class Expression;
    class ExpressionContext;
    class ExpressionFieldPath;
    class ExpressionObject;
    class Matcher;
    class Shard;
    class ShardChunkManager;

    class DocumentSource :
        public IntrusiveCounterUnsigned,
        public StringWriter {
    public:
        virtual ~DocumentSource();

        // virtuals from StringWriter
        virtual void writeString(stringstream &ss) const;

        /**
           Set the step for a user-specified pipeline step.

           The step is used for diagnostics.

           @param step step number 0 to n.
        */
        void setPipelineStep(int step);

        /**
           Get the user-specified pipeline step.

           @returns the step number, or -1 if it has never been set
        */
        int getPipelineStep() const;

        /**
          Is the source at EOF?

          @returns true if the source has no more Documents to return.
        */
        virtual bool eof() = 0;

        /**
          Advance the state of the DocumentSource so that it will return the
          next Document.

          The default implementation returns false, after checking for
          interrupts.  Derived classes can call the default implementation
          in their own implementations in order to check for interrupts.

          @returns whether there is another document to fetch, i.e., whether or
            not getCurrent() will succeed.  This default implementation always
            returns false.
        */
        virtual bool advance();

        /**
          Advance the source, and return the next Expression.

          @returns the current Document
          TODO throws an exception if there are no more expressions to return.
        */
        virtual intrusive_ptr<Document> getCurrent() = 0;

        /**
         * Inform the source that it is no longer needed and may release its resources.  After
         * dispose() is called the source must still be able to handle iteration requests, but may
         * become eof().
         * NOTE: For proper mutex yielding, dispose() must be called on any DocumentSource that will
         * not be advanced until eof(), see SERVER-6123.
         */
        virtual void dispose();

        /**
           Get the source's name.

           @returns the string name of the source as a constant string;
             this is static, and there's no need to worry about adopting it
         */
        virtual const char *getSourceName() const;

        /**
          Set the underlying source this source should use to get Documents
          from.

          It is an error to set the source more than once.  This is to
          prevent changing sources once the original source has been started;
          this could break the state maintained by the DocumentSource.

          This pointer is not reference counted because that has led to
          some circular references.  As a result, this doesn't keep
          sources alive, and is only intended to be used temporarily for
          the lifetime of a Pipeline::run().

          @param pSource the underlying source to use
         */
        virtual void setSource(DocumentSource *pSource);

        /**
          Attempt to coalesce this DocumentSource with its successor in the
          document processing pipeline.  If successful, the successor
          DocumentSource should be removed from the pipeline and discarded.

          If successful, this operation can be applied repeatedly, in an
          attempt to coalesce several sources together.

          The default implementation is to do nothing, and return false.

          @param pNextSource the next source in the document processing chain.
          @returns whether or not the attempt to coalesce was successful or not;
            if the attempt was not successful, nothing has been changed
         */
        virtual bool coalesce(const intrusive_ptr<DocumentSource> &pNextSource);

        /**
          Optimize the pipeline operation, if possible.  This is a local
          optimization that only looks within this DocumentSource.  For best
          results, first coalesce compatible sources using coalesce().

          This is intended for any operations that include expressions, and
          provides a hook for those to optimize those operations.

          The default implementation is to do nothing.
         */
        virtual void optimize();

        /**
           Adjust dependencies according to the needs of this source.

           $$$ MONGO_LATER_SERVER_4644
           @param pTracker the dependency tracker
         */
        virtual void manageDependencies(
            const intrusive_ptr<DependencyTracker> &pTracker);

        /**
          Add the DocumentSource to the array builder.

          The default implementation calls sourceToBson() in order to
          convert the inner part of the object which will be added to the
          array being built here.

          @param pBuilder the array builder to add the operation to.
          @param explain create explain output
         */
        virtual void addToBsonArray(BSONArrayBuilder *pBuilder,
            bool explain = false) const;
        
    protected:
        /**
           Base constructor.
         */
        DocumentSource(const intrusive_ptr<ExpressionContext> &pExpCtx);

        /**
          Create an object that represents the document source.  The object
          will have a single field whose name is the source's name.  This
          will be used by the default implementation of addToBsonArray()
          to add this object to a pipeline being represented in BSON.

          @param pBuilder a blank object builder to write to
          @param explain create explain output
         */
        virtual void sourceToBson(BSONObjBuilder *pBuilder,
                                  bool explain) const = 0;

        /*
          Most DocumentSources have an underlying source they get their data
          from.  This is a convenience for them.

          The default implementation of setSource() sets this; if you don't
          need a source, override that to verify().  The default is to
          verify() if this has already been set.
        */
        DocumentSource *pSource;

        /*
          The zero-based user-specified pipeline step.  Used for diagnostics.
          Will be set to -1 for artificial pipeline steps that were not part
          of the original user specification.
         */
        int step;

        intrusive_ptr<ExpressionContext> pExpCtx;

        /*
          for explain: # of rows returned by this source

          This is *not* unsigned so it can be passed to BSONObjBuilder.append().
         */
        long long nRowsOut;
    };

    /** This class marks DocumentSources that should be split between the router and the shards
     *  See Pipeline::splitForSharded() for details
     */
    class SplittableDocumentSource : public DocumentSource {
    public:
        /** returns a source to be run on the shards.
         *  if NULL, don't run on shards
         */
        virtual intrusive_ptr<DocumentSource> getShardSource() = 0;

        /** returns a source that combines results from shards.
         *  if NULL, don't run on router
         */
        virtual intrusive_ptr<DocumentSource> getRouterSource() = 0;
    protected:
        SplittableDocumentSource(intrusive_ptr<ExpressionContext> ctx) :DocumentSource(ctx) {}
    };


    class DocumentSourceBsonArray :
        public DocumentSource {
    public:
        // virtuals from DocumentSource
        virtual ~DocumentSourceBsonArray();
        virtual bool eof();
        virtual bool advance();
        virtual intrusive_ptr<Document> getCurrent();
        virtual void setSource(DocumentSource *pSource);

        /**
          Create a document source based on a BSON array.

          This is usually put at the beginning of a chain of document sources
          in order to fetch data from the database.

          CAUTION:  the BSON is not read until the source is used.  Any
          elements that appear after these documents must not be read until
          this source is exhausted.

          @param pBsonElement the BSON array to treat as a document source
          @param pExpCtx the expression context for the pipeline
          @returns the newly created document source
        */
        static intrusive_ptr<DocumentSourceBsonArray> create(
            BSONElement *pBsonElement,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

    protected:
        // virtuals from DocumentSource
        virtual void sourceToBson(BSONObjBuilder *pBuilder, bool explain) const;

    private:
        DocumentSourceBsonArray(BSONElement *pBsonElement,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        BSONObj embeddedObject;
        BSONObjIterator arrayIterator;
        BSONElement currentElement;
        bool haveCurrent;
    };

    
    class DocumentSourceCommandShards :
        public DocumentSource {
    public:
        // virtuals from DocumentSource
        virtual ~DocumentSourceCommandShards();
        virtual bool eof();
        virtual bool advance();
        virtual intrusive_ptr<Document> getCurrent();
        virtual void setSource(DocumentSource *pSource);

        /* convenient shorthand for a commonly used type */
        typedef map<Shard, BSONObj> ShardOutput;

        /**
          Create a DocumentSource that wraps the output of many shards

          @param shardOutput output from the individual shards
          @param pExpCtx the expression context for the pipeline
          @returns the newly created DocumentSource
         */
        static intrusive_ptr<DocumentSourceCommandShards> create(
            const ShardOutput& shardOutput,
            const intrusive_ptr<ExpressionContext>& pExpCtx);

    protected:
        // virtuals from DocumentSource
        virtual void sourceToBson(BSONObjBuilder *pBuilder, bool explain) const;

    private:
        DocumentSourceCommandShards(const ShardOutput& shardOutput,
            const intrusive_ptr<ExpressionContext>& pExpCtx);

        /**
          Advance to the next document, setting pCurrent appropriately.

          Adjusts pCurrent, pBsonSource, and iterator, as needed.  On exit,
          pCurrent is the Document to return, or NULL.  If NULL, this
          indicates there is nothing more to return.
         */
        void getNextDocument();

        bool newSource; // set to true for the first item of a new source
        intrusive_ptr<DocumentSourceBsonArray> pBsonSource;
        intrusive_ptr<Document> pCurrent;
        ShardOutput::const_iterator iterator;
        ShardOutput::const_iterator listEnd;
    };


    /**
     * Constructs and returns Documents from the BSONObj objects produced by a supplied Cursor.
     * An object of this type may only be used by one thread, see SERVER-6123.
     */
    class DocumentSourceCursor :
        public DocumentSource {
    public:
        /**
         * Holds a Cursor and all associated state required to access the cursor.  An object of this
         * type may only be used by one thread.
         */
        struct CursorWithContext {
            /** Takes a read lock that will be held for the lifetime of the object. */
            CursorWithContext( const string& ns );

            // Must be the first struct member for proper construction and destruction, as other
            // members may depend on the read lock it acquires.
            Client::ReadContext _readContext;
            shared_ptr<ShardChunkManager> _chunkMgr;
            ClientCursor::Holder _cursor;
        };

        // virtuals from DocumentSource
        virtual ~DocumentSourceCursor();
        virtual bool eof();
        virtual bool advance();
        virtual intrusive_ptr<Document> getCurrent();
        virtual void setSource(DocumentSource *pSource);
        virtual void manageDependencies(
            const intrusive_ptr<DependencyTracker> &pTracker);

        /**
         * Release the Cursor and the read lock it requires, but without changing the other data.
         * Releasing the lock is required for proper concurrency, see SERVER-6123.  This
         * functionality is also used by the explain version of pipeline execution.
         */
        virtual void dispose();

        /**
          Create a document source based on a cursor.

          This is usually put at the beginning of a chain of document sources
          in order to fetch data from the database.

          @param pCursor the cursor to use to fetch data
          @param pExpCtx the expression context for the pipeline
        */
        static intrusive_ptr<DocumentSourceCursor> create(
            const shared_ptr<CursorWithContext>& cursorWithContext,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        /*
          Record the namespace.  Required for explain.

          @param namespace the namespace
        */
        void setNamespace(const string &ns);

        /*
          Record the query that was specified for the cursor this wraps, if
          any.

          This should be captured after any optimizations are applied to
          the pipeline so that it reflects what is really used.

          This gets used for explain output.

          @param pBsonObj the query to record
         */
        void setQuery(const shared_ptr<BSONObj> &pBsonObj);

        /*
          Record the sort that was specified for the cursor this wraps, if
          any.

          This should be captured after any optimizations are applied to
          the pipeline so that it reflects what is really used.

          This gets used for explain output.

          @param pBsonObj the sort to record
         */
        void setSort(const shared_ptr<BSONObj> &pBsonObj);

        void setProjection(BSONObj projection);
    protected:
        // virtuals from DocumentSource
        virtual void sourceToBson(BSONObjBuilder *pBuilder, bool explain) const;

    private:
        DocumentSourceCursor(
            const shared_ptr<CursorWithContext>& cursorWithContext,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        void findNext();

        intrusive_ptr<Document> pCurrent;

        string ns; // namespace

        /*
          The bson dependencies must outlive the Cursor wrapped by this
          source.  Therefore, bson dependencies must appear before pCursor
          in order cause its destructor to be called *after* pCursor's.
         */
        shared_ptr<BSONObj> pQuery;
        shared_ptr<BSONObj> pSort;
        shared_ptr<Projection> _projection; // shared with pClientCursor

        shared_ptr<CursorWithContext> _cursorWithContext;

        ClientCursor::Holder& cursor();
        const ShardChunkManager* chunkMgr() { return _cursorWithContext->_chunkMgr.get(); }

        bool canUseCoveredIndex();

        /*
          Yield the cursor sometimes.

          If the state of the world changed during the yield such that we
          are unable to continue execution of the query, this will release the
          client cursor, and throw an error.  NOTE This differs from the
          behavior of most other operations, see SERVER-2454.
         */
        void yieldSometimes();

        /*
          This document source hangs on to the dependency tracker when it
          gets it so that it can be used for selective reification of
          fields in order to avoid fields that are not required through the
          pipeline.
         */
        intrusive_ptr<DependencyTracker> pDependencies;
    };


    /*
      This contains all the basic mechanics for filtering a stream of
      Documents, except for the actual predicate evaluation itself.  This was
      factored out so we could create DocumentSources that use both Matcher
      style predicates as well as full Expressions.
     */
    class DocumentSourceFilterBase :
        public DocumentSource {
    public:
        // virtuals from DocumentSource
        virtual ~DocumentSourceFilterBase();
        virtual bool eof();
        virtual bool advance();
        virtual intrusive_ptr<Document> getCurrent();

        /**
          Create a BSONObj suitable for Matcher construction.

          This is used after filter analysis has moved as many filters to
          as early a point as possible in the document processing pipeline.
          See db/Matcher.h and the associated wiki documentation for the
          format.  This conversion is used to move back to the low-level
          find() Cursor mechanism.

          @param pBuilder the builder to write to
         */
        virtual void toMatcherBson(BSONObjBuilder *pBuilder) const = 0;

    protected:
        DocumentSourceFilterBase(
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        /**
          Test the given document against the predicate and report if it
          should be accepted or not.

          @param pDocument the document to test
          @returns true if the document matches the filter, false otherwise
         */
        virtual bool accept(const intrusive_ptr<Document> &pDocument) const = 0;

    private:

        void findNext();

        bool unstarted;
        bool hasNext;
        intrusive_ptr<Document> pCurrent;
    };


    class DocumentSourceFilter :
        public DocumentSourceFilterBase {
    public:
        // virtuals from DocumentSource
        virtual ~DocumentSourceFilter();
        virtual bool coalesce(const intrusive_ptr<DocumentSource> &pNextSource);
        virtual void optimize();
        virtual const char *getSourceName() const;

        /**
          Create a filter.

          @param pBsonElement the raw BSON specification for the filter
          @param pExpCtx the expression context for the pipeline
          @returns the filter
         */
        static intrusive_ptr<DocumentSource> createFromBson(
            BSONElement *pBsonElement,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        /**
          Create a filter.

          @param pFilter the expression to use to filter
          @param pExpCtx the expression context for the pipeline
          @returns the filter
         */
        static intrusive_ptr<DocumentSourceFilter> create(
            const intrusive_ptr<Expression> &pFilter,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        /**
          Create a BSONObj suitable for Matcher construction.

          This is used after filter analysis has moved as many filters to
          as early a point as possible in the document processing pipeline.
          See db/Matcher.h and the associated wiki documentation for the
          format.  This conversion is used to move back to the low-level
          find() Cursor mechanism.

          @param pBuilder the builder to write to
         */
        void toMatcherBson(BSONObjBuilder *pBuilder) const;

        static const char filterName[];

    protected:
        // virtuals from DocumentSource
        virtual void sourceToBson(BSONObjBuilder *pBuilder, bool explain) const;

        // virtuals from DocumentSourceFilterBase
        virtual bool accept(const intrusive_ptr<Document> &pDocument) const;

    private:
        DocumentSourceFilter(const intrusive_ptr<Expression> &pFilter,
                             const intrusive_ptr<ExpressionContext> &pExpCtx);

        intrusive_ptr<Expression> pFilter;
    };


    class DocumentSourceGroup :
        public SplittableDocumentSource {
    public:
        // virtuals from DocumentSource
        virtual ~DocumentSourceGroup();
        virtual bool eof();
        virtual bool advance();
        virtual const char *getSourceName() const;
        virtual intrusive_ptr<Document> getCurrent();

        /**
          Create a new grouping DocumentSource.
          
          @param pExpCtx the expression context for the pipeline
          @returns the DocumentSource
         */
        static intrusive_ptr<DocumentSourceGroup> create(
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        /**
          Set the Id Expression.

          Documents that pass through the grouping Document are grouped
          according to this key.  This will generate the id_ field in the
          result documents.

          @param pExpression the group key
         */
        void setIdExpression(const intrusive_ptr<Expression> &pExpression);

        /**
          Add an accumulator.

          Accumulators become fields in the Documents that result from
          grouping.  Each unique group document must have it's own
          accumulator; the accumulator factory is used to create that.

          @param fieldName the name the accumulator result will have in the
                result documents
          @param pAccumulatorFactory used to create the accumulator for the
                group field
         */
        void addAccumulator(string fieldName,
                            intrusive_ptr<Accumulator> (*pAccumulatorFactory)(
                            const intrusive_ptr<ExpressionContext> &),
                            const intrusive_ptr<Expression> &pExpression);

        /**
          Create a grouping DocumentSource from BSON.

          This is a convenience method that uses the above, and operates on
          a BSONElement that has been deteremined to be an Object with an
          element named $group.

          @param pBsonElement the BSONELement that defines the group
          @param pExpCtx the expression context
          @returns the grouping DocumentSource
         */
        static intrusive_ptr<DocumentSource> createFromBson(
            BSONElement *pBsonElement,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        // Virtuals for SplittableDocumentSource
        virtual intrusive_ptr<DocumentSource> getShardSource();
        virtual intrusive_ptr<DocumentSource> getRouterSource();

        static const char groupName[];

    protected:
        // virtuals from DocumentSource
        virtual void sourceToBson(BSONObjBuilder *pBuilder, bool explain) const;

    private:
        DocumentSourceGroup(const intrusive_ptr<ExpressionContext> &pExpCtx);

        /*
          Before returning anything, this source must fetch everything from
          the underlying source and group it.  populate() is used to do that
          on the first call to any method on this source.  The populated
          boolean indicates that this has been done.
         */
        void populate();
        bool populated;

        intrusive_ptr<Expression> pIdExpression;

        typedef boost::unordered_map<intrusive_ptr<const Value>,
            vector<intrusive_ptr<Accumulator> >, Value::Hash> GroupsType;
        GroupsType groups;

        /*
          The field names for the result documents and the accumulator
          factories for the result documents.  The Expressions are the
          common expressions used by each instance of each accumulator
          in order to find the right-hand side of what gets added to the
          accumulator.  Note that each of those is the same for each group,
          so we can share them across all groups by adding them to the
          accumulators after we use the factories to make a new set of
          accumulators for each new group.

          These three vectors parallel each other.
        */
        vector<string> vFieldName;
        vector<intrusive_ptr<Accumulator> (*)(
            const intrusive_ptr<ExpressionContext> &)> vpAccumulatorFactory;
        vector<intrusive_ptr<Expression> > vpExpression;


        intrusive_ptr<Document> makeDocument(
            const GroupsType::iterator &rIter);

        GroupsType::iterator groupsIterator;
        intrusive_ptr<Document> pCurrent;
    };


    class DocumentSourceMatch :
        public DocumentSourceFilterBase {
    public:
        // virtuals from DocumentSource
        virtual ~DocumentSourceMatch();
        virtual const char *getSourceName() const;
        virtual void manageDependencies(
            const intrusive_ptr<DependencyTracker> &pTracker);

        /**
          Create a filter.

          @param pBsonElement the raw BSON specification for the filter
          @returns the filter
         */
        static intrusive_ptr<DocumentSource> createFromBson(
            BSONElement *pBsonElement,
            const intrusive_ptr<ExpressionContext> &pCtx);

        /**
          Create a BSONObj suitable for Matcher construction.

          This is used after filter analysis has moved as many filters to
          as early a point as possible in the document processing pipeline.
          See db/Matcher.h and the associated wiki documentation for the
          format.  This conversion is used to move back to the low-level
          find() Cursor mechanism.

          @param pBuilder the builder to write to
         */
        void toMatcherBson(BSONObjBuilder *pBuilder) const;

        static const char matchName[];

    protected:
        // virtuals from DocumentSource
        virtual void sourceToBson(BSONObjBuilder *pBuilder, bool explain) const;

        // virtuals from DocumentSourceFilterBase
        virtual bool accept(const intrusive_ptr<Document> &pDocument) const;

    private:
        DocumentSourceMatch(const BSONObj &query,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        Matcher matcher;
    };


    class DocumentSourceOut :
        public DocumentSource {
    public:
        // virtuals from DocumentSource
        virtual ~DocumentSourceOut();
        virtual bool eof();
        virtual bool advance();
        virtual const char *getSourceName() const;
        virtual intrusive_ptr<Document> getCurrent();

        /**
          Create a document source for output and pass-through.

          This can be put anywhere in a pipeline and will store content as
          well as pass it on.

          @param pBsonElement the raw BSON specification for the source
          @param pExpCtx the expression context for the pipeline
          @returns the newly created document source
        */
        static intrusive_ptr<DocumentSourceOut> createFromBson(
            BSONElement *pBsonElement,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        static const char outName[];

    protected:
        // virtuals from DocumentSource
        virtual void sourceToBson(BSONObjBuilder *pBuilder, bool explain) const;

    private:
        DocumentSourceOut(BSONElement *pBsonElement,
            const intrusive_ptr<ExpressionContext> &pExpCtx);
    };

    
    class DocumentSourceProject :
        public DocumentSource {
    public:
        // virtuals from DocumentSource
        virtual ~DocumentSourceProject();
        virtual bool eof();
        virtual bool advance();
        virtual const char *getSourceName() const;
        virtual intrusive_ptr<Document> getCurrent();
        virtual void optimize();
        virtual void manageDependencies(
            const intrusive_ptr<DependencyTracker> &pTracker);

        /**
          Include a field path in a projection.

          @param fieldPath the path of the field to include
        */
        void includePath(const string &fieldPath);

        /**
          Exclude a field path from the projection.

          @param fieldPath the path of the field to exclude
         */
        void excludePath(const string &fieldPath);

        /**
          Add an output Expression in the projection.

          BSON document fields are ordered, so the new field will be
          appended to the existing set.

          @param fieldName the name of the field as it will appear
          @param pExpression the expression used to compute the field
        */
        void addField(const string &fieldName,
                      const intrusive_ptr<Expression> &pExpression);

        /**
          Create a new projection DocumentSource from BSON.

          This is a convenience for directly handling BSON, and relies on the
          above methods.

          @param pBsonElement the BSONElement with an object named $project
          @param pExpCtx the expression context for the pipeline
          @returns the created projection
         */
        static intrusive_ptr<DocumentSource> createFromBson(
            BSONElement *pBsonElement,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        static const char projectName[];

        /** projection as specified by the user */
        BSONObj getRaw() const { return _raw; }

        /** true if just include/exclude, no renames */
        bool isSimple() const { return _isSimple; }

        /** called by PipelineD::prepareCursorSource in debug builds if it would remove this Projection */
        void setWouldBeRemoved() { _wouldBeRemoved = true; }

    protected:
        // virtuals from DocumentSource
        virtual void sourceToBson(BSONObjBuilder *pBuilder, bool explain) const;

    private:
        DocumentSourceProject(const intrusive_ptr<ExpressionContext> &pExpCtx);

        // configuration state
        bool excludeId;
        intrusive_ptr<ExpressionObject> pEO;
        BSONObj _raw;
        bool _isSimple;
        bool _wouldBeRemoved; // only used by debug builds

        /*
          Utility object used by manageDependencies().

          Removes dependencies from a DependencyTracker.
         */
        class DependencyRemover :
            public ExpressionObject::PathSink {
        public:
            // virtuals from PathSink
            virtual void path(const string &path, bool include);

            /*
              Constructor.

              Captures a reference to the smart pointer to the DependencyTracker
              that this will remove dependencies from via
              ExpressionObject::emitPaths().

              @param pTracker reference to the smart pointer to the
                DependencyTracker
             */
            DependencyRemover(const intrusive_ptr<DependencyTracker> &pTracker);

        private:
            const intrusive_ptr<DependencyTracker> &pTracker;
        };

        /*
          Utility object used by manageDependencies().

          Checks dependencies to see if they are present.  If not, then
          throws a user error.
         */
        class DependencyChecker :
            public ExpressionObject::PathSink {
        public:
            // virtuals from PathSink
            virtual void path(const string &path, bool include);

            /*
              Constructor.

              Captures a reference to the smart pointer to the DependencyTracker
              that this will check dependencies from from
              ExpressionObject::emitPaths() to see if they are required.

              @param pTracker reference to the smart pointer to the
                DependencyTracker
              @param pThis the projection that is making this request
             */
            DependencyChecker(
                const intrusive_ptr<DependencyTracker> &pTracker,
                const DocumentSourceProject *pThis);

        private:
            const intrusive_ptr<DependencyTracker> &pTracker;
            const DocumentSourceProject *pThis;
        };
    };


    class DocumentSourceSort :
        public SplittableDocumentSource {
    public:
        // virtuals from DocumentSource
        virtual ~DocumentSourceSort();
        virtual bool eof();
        virtual bool advance();
        virtual const char *getSourceName() const;
        virtual intrusive_ptr<Document> getCurrent();
        virtual void manageDependencies(
            const intrusive_ptr<DependencyTracker> &pTracker);
        /*
          TODO
          Adjacent sorts should reduce to the last sort.
        virtual bool coalesce(const intrusive_ptr<DocumentSource> &pNextSource);
        */

        /**
          Create a new sorting DocumentSource.
          
          @param pExpCtx the expression context for the pipeline
          @returns the DocumentSource
         */
        static intrusive_ptr<DocumentSourceSort> create(
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        // Virtuals for SplittableDocumentSource
        // All work for sort is done in router currently
        // TODO: do partial sorts on the shards then merge in the router
        //       Not currently possible due to DocumentSource's cursor-like interface
        virtual intrusive_ptr<DocumentSource> getShardSource() { return NULL; }
        virtual intrusive_ptr<DocumentSource> getRouterSource() { return this; }

        /**
          Add sort key field.

          Adds a sort key field to the key being built up.  A concatenated
          key is built up by calling this repeatedly.

          @param fieldPath the field path to the key component
          @param ascending if true, use the key for an ascending sort,
            otherwise, use it for descending
        */
        void addKey(const string &fieldPath, bool ascending);

        /**
          Write out an object whose contents are the sort key.

          @param pBuilder initialized object builder.
          @param fieldPrefix specify whether or not to include the field prefix
         */
        void sortKeyToBson(BSONObjBuilder *pBuilder, bool usePrefix) const;

        /**
          Create a sorting DocumentSource from BSON.

          This is a convenience method that uses the above, and operates on
          a BSONElement that has been deteremined to be an Object with an
          element named $group.

          @param pBsonElement the BSONELement that defines the group
          @param pExpCtx the expression context for the pipeline
          @returns the grouping DocumentSource
         */
        static intrusive_ptr<DocumentSource> createFromBson(
            BSONElement *pBsonElement,
            const intrusive_ptr<ExpressionContext> &pExpCtx);


        static const char sortName[];

    protected:
        // virtuals from DocumentSource
        virtual void sourceToBson(BSONObjBuilder *pBuilder, bool explain) const;

    private:
        DocumentSourceSort(const intrusive_ptr<ExpressionContext> &pExpCtx);

        /*
          Before returning anything, this source must fetch everything from
          the underlying source and group it.  populate() is used to do that
          on the first call to any method on this source.  The populated
          boolean indicates that this has been done.
         */
        void populate();
        bool populated;
        long long count;

        /* these two parallel each other */
        typedef vector<intrusive_ptr<ExpressionFieldPath> > SortPaths;
        SortPaths vSortKey;
        vector<bool> vAscending;

        /*
          Compare two documents according to the specified sort key.

          @param rL reference to the left document
          @param rR reference to the right document
          @returns a number less than, equal to, or greater than zero,
            indicating pL < pR, pL == pR, or pL > pR, respectively
         */
        int compare(const intrusive_ptr<Document> &pL,
                    const intrusive_ptr<Document> &pR);

        /*
          This is a utility class just for the STL sort that is done
          inside.
         */
        class Comparator {
        public:
            bool operator()(
                const intrusive_ptr<Document> &pL,
                const intrusive_ptr<Document> &pR) {
                return (pSort->compare(pL, pR) < 0);
            }

            inline Comparator(DocumentSourceSort *pS):
                pSort(pS) {
            }

        private:
            DocumentSourceSort *pSort;
        };

        typedef vector<intrusive_ptr<Document> > VectorType;
        VectorType documents;

        VectorType::iterator docIterator;
        intrusive_ptr<Document> pCurrent;
    };


    class DocumentSourceLimit :
        public DocumentSource {
    public:
        // virtuals from DocumentSource
        virtual ~DocumentSourceLimit();
        virtual bool eof();
        virtual bool advance();
        virtual intrusive_ptr<Document> getCurrent();
        virtual const char *getSourceName() const;
        virtual bool coalesce(const intrusive_ptr<DocumentSource> &pNextSource);

        /**
          Create a new limiting DocumentSource.

          @param pExpCtx the expression context for the pipeline
          @returns the DocumentSource
         */
        static intrusive_ptr<DocumentSourceLimit> create(
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        /**
          Create a limiting DocumentSource from BSON.

          This is a convenience method that uses the above, and operates on
          a BSONElement that has been deteremined to be an Object with an
          element named $limit.

          @param pBsonElement the BSONELement that defines the limit
          @param pExpCtx the expression context
          @returns the grouping DocumentSource
         */
        static intrusive_ptr<DocumentSource> createFromBson(
            BSONElement *pBsonElement,
            const intrusive_ptr<ExpressionContext> &pExpCtx);


        static const char limitName[];

    protected:
        // virtuals from DocumentSource
        virtual void sourceToBson(BSONObjBuilder *pBuilder, bool explain) const;

    private:
        DocumentSourceLimit(
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        long long limit;
        long long count;
        intrusive_ptr<Document> pCurrent;
    };

    class DocumentSourceSkip :
        public DocumentSource {
    public:
        // virtuals from DocumentSource
        virtual ~DocumentSourceSkip();
        virtual bool eof();
        virtual bool advance();
        virtual intrusive_ptr<Document> getCurrent();
        virtual const char *getSourceName() const;
        virtual bool coalesce(const intrusive_ptr<DocumentSource> &pNextSource);

        /**
          Create a new skipping DocumentSource.

          @param pExpCtx the expression context
          @returns the DocumentSource
         */
        static intrusive_ptr<DocumentSourceSkip> create(
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        /**
          Create a skipping DocumentSource from BSON.

          This is a convenience method that uses the above, and operates on
          a BSONElement that has been deteremined to be an Object with an
          element named $skip.

          @param pBsonElement the BSONELement that defines the skip
          @param pExpCtx the expression context
          @returns the grouping DocumentSource
         */
        static intrusive_ptr<DocumentSource> createFromBson(
            BSONElement *pBsonElement,
            const intrusive_ptr<ExpressionContext> &pExpCtx);


        static const char skipName[];

    protected:
        // virtuals from DocumentSource
        virtual void sourceToBson(BSONObjBuilder *pBuilder, bool explain) const;

    private:
        DocumentSourceSkip(const intrusive_ptr<ExpressionContext> &pExpCtx);

        /*
          Skips initial documents.
         */
        void skipper();

        long long skip;
        long long count;
        intrusive_ptr<Document> pCurrent;
    };


    class DocumentSourceUnwind :
        public DocumentSource {
    public:
        // virtuals from DocumentSource
        virtual ~DocumentSourceUnwind();
        virtual bool eof();
        virtual bool advance();
        virtual const char *getSourceName() const;
        virtual intrusive_ptr<Document> getCurrent();
        virtual void manageDependencies(
            const intrusive_ptr<DependencyTracker> &pTracker);

        /**
          Create a new DocumentSource that can implement unwind.

          @param pExpCtx the expression context for the pipeline
          @returns the projection DocumentSource
        */
        static intrusive_ptr<DocumentSourceUnwind> create(
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        /**
          Specify the field to unwind.  There must be exactly one before
          the pipeline begins execution.

          @param rFieldPath - path to the field to unwind
        */
        void unwindField(const FieldPath &rFieldPath);

        /**
          Create a new projection DocumentSource from BSON.

          This is a convenience for directly handling BSON, and relies on the
          above methods.

          @param pBsonElement the BSONElement with an object named $project
          @param pExpCtx the expression context for the pipeline
          @returns the created projection
         */
        static intrusive_ptr<DocumentSource> createFromBson(
            BSONElement *pBsonElement,
            const intrusive_ptr<ExpressionContext> &pExpCtx);

        static const char unwindName[];

    protected:
        // virtuals from DocumentSource
        virtual void sourceToBson(BSONObjBuilder *pBuilder, bool explain) const;

    private:
        DocumentSourceUnwind(const intrusive_ptr<ExpressionContext> &pExpCtx);

        // configuration state
        FieldPath unwindPath;

        vector<int> fieldIndex; /* for the current document, the indices
                                   leading down to the field being unwound */

        // iteration state
        intrusive_ptr<Document> pNoUnwindDocument;
                                              // document to return, pre-unwind
        intrusive_ptr<const Value> pUnwindArray; // field being unwound
        intrusive_ptr<ValueIterator> pUnwinder; // iterator used for unwinding
        intrusive_ptr<const Value> pUnwindValue; // current value

        /*
          Clear all the state related to unwinding an array.
         */
        void resetArray();

        /*
          Clone the current document being unwound.

          This is a partial deep clone.  Because we're going to replace the
          value at the end, we have to replace everything along the path
          leading to that in order to not share that change with any other
          clones (or the original) that we've made.

          This expects pUnwindValue to have been set by a prior call to
          advance().  However, pUnwindValue may also be NULL, in which case
          the field will be removed -- this is the action for an empty
          array.

          @returns a partial deep clone of pNoUnwindDocument
         */
        intrusive_ptr<Document> clonePath() const;
    };

}


/* ======================= INLINED IMPLEMENTATIONS ========================== */

namespace mongo {

    inline void DocumentSource::setPipelineStep(int s) {
        step = s;
    }

    inline int DocumentSource::getPipelineStep() const {
        return step;
    }
    
    inline void DocumentSourceGroup::setIdExpression(
        const intrusive_ptr<Expression> &pExpression) {
        pIdExpression = pExpression;
    }

    inline DocumentSourceProject::DependencyRemover::DependencyRemover(
        const intrusive_ptr<DependencyTracker> &pT):
        pTracker(pT) {
    }

    inline DocumentSourceProject::DependencyChecker::DependencyChecker(
        const intrusive_ptr<DependencyTracker> &pTrack,
        const DocumentSourceProject *pT):
        pTracker(pTrack),
        pThis(pT) {
    }

    inline void DocumentSourceUnwind::resetArray() {
        pNoUnwindDocument.reset();
        pUnwindArray.reset();
        pUnwinder.reset();
        pUnwindValue.reset();
    }

}
