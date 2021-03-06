#include "db_table.hpp"
#include "db_segment.hpp"
#include "intkey_index.hpp"
#include "zip_int_store.hpp"
#include "fixed_len_key_index.hpp"
#include "fixed_len_store.hpp"
#include "appendonly.hpp"
#include <terark/util/autoclose.hpp>
#include <terark/io/FileStream.hpp>
#include <terark/io/StreamBuffer.hpp>
#include <terark/io/DataIO.hpp>
#include <terark/io/MemStream.hpp>
#include <terark/lcast.hpp>
#include <terark/num_to_str.hpp>
#include <terark/util/mmap.hpp>
#include <terark/util/sortable_strvec.hpp>
#include <terark/util/truncate_file.hpp>

//#define TERARK_DB_ENABLE_DFA_META
#if defined(TERARK_DB_ENABLE_DFA_META)
#include <terark/fsa/nest_trie_dawg.hpp>
#endif

#if defined(_MSC_VER)
	#include <io.h>
#else
	#include <unistd.h>
#endif
#include <fcntl.h>
#include <float.h>

#include "json.hpp"

#include <boost/scope_exit.hpp>

namespace terark { namespace db {

namespace fs = boost::filesystem;


ReadableSegment::ReadableSegment() {
	m_delcnt = 0;
	m_tobeDel = false;
	m_isDirty = false;
	m_isFreezed = false;
	m_hasLockFreePointSearch = true;
	m_bookUpdates = false;
	m_withPurgeBits = false;
	m_isPurgedMmap = nullptr;
}
ReadableSegment::~ReadableSegment() {
	if (m_isDelMmap) {
		closeIsDel();
	}
	else if (m_isDirty && !m_tobeDel && !m_segDir.empty()) {
		saveIsDel(m_segDir);
	}
	m_indices.clear(); // destroy index objects
	m_colgroups.clear();
	assert(!m_segDir.empty());
	if (m_tobeDel && !m_segDir.empty()) {
		fprintf(stderr, "INFO: remove: %s\n", m_segDir.string().c_str());
		try { boost::filesystem::remove_all(m_segDir); }
		catch (const std::exception& ex) {
			fprintf(stderr
				, "ERROR: ReadableSegment::~ReadableSegment(): ex.what = %s\n"
				, ex.what());
		// windows can not delete a hardlink when another hardlink
		// to the same file is in use
		//	TERARK_IF_DEBUG(abort(),;);
		}
	}
}

ReadonlySegment* ReadableSegment::getReadonlySegment() const {
	return nullptr;
}
WritableSegment* ReadableSegment::getWritableSegment() const {
	return nullptr;
}

void ReadableSegment::deleteSegment() {
	assert(!m_segDir.empty());
	m_tobeDel = true;
}

llong ReadableSegment::numDataRows() const {
	return m_isDel.size();
}

void ReadableSegment::saveIsDel(PathRef dir) const {
	assert(m_isDel.popcnt() == m_delcnt);
	if (m_isDelMmap && dir == m_segDir) {
		// need not to save, mmap is sys memory
		return;
	}
	fs::path isDelFpath = dir / "IsDel";
	fs::path tmpFpath = isDelFpath + ".tmp";
	{
		NativeDataOutput<FileStream> file;
		file.open(tmpFpath.string().c_str(), "wb");
		file << uint64_t(m_isDel.size());
		file.ensureWrite(m_isDel.bldata(), m_isDel.mem_size());
	}
	fs::rename(tmpFpath, isDelFpath);
}

void ReadableSegment::loadIsDel(PathRef dir) {
	if (m_isDelMmap) {
		m_isDel.risk_release_ownership();
		m_isDelMmap = nullptr;
	}
	else {
		m_isDel.clear(); // free memory
	}
	m_delcnt = 0;
	m_isDelMmap = loadIsDel_aux(dir, m_isDel);
	m_delcnt = m_isDel.popcnt();
}

byte* ReadableSegment::loadIsDel_aux(PathRef segDir, febitvec& isDel) const {
	fs::path isDelFpath = segDir / "IsDel";
	size_t bytes = 0;
	bool writable = true;
	std::string fpath = isDelFpath.string();
	byte* isDelMmap = (byte*)mmap_load(fpath, &bytes, writable);
	uint64_t rowNum = ((uint64_t*)isDelMmap)[0];
	isDel.risk_mmap_from(isDelMmap + 8, bytes - 8);
	assert(isDel.size() >= rowNum);
	isDel.risk_set_size(size_t(rowNum));
	return isDelMmap;
}

void ReadableSegment::closeIsDel() {
	if (m_isDelMmap) {
		size_t bitBytes = m_isDel.capacity()/8;
		mmap_close(m_isDelMmap, sizeof(uint64_t) + bitBytes);
		m_isDel.risk_release_ownership();
		m_isDelMmap = NULL;
	}
	else {
		m_isDel.clear();
	}
}

void ReadableSegment::openIndices(PathRef segDir) {
	if (!m_indices.empty()) {
		THROW_STD(invalid_argument, "m_indices must be empty");
	}
	m_indices.resize(m_schema->getIndexNum());
	for (size_t i = 0; i < m_schema->getIndexNum(); ++i) {
		const Schema& schema = m_schema->getIndexSchema(i);
		fs::path path = segDir / ("index-" + schema.m_name);
		m_indices[i] = this->openIndex(schema, path.string());
	}
}

void ReadableSegment::saveIndices(PathRef segDir) const {
	assert(m_indices.size() == m_schema->getIndexNum());
	for (size_t i = 0; i < m_indices.size(); ++i) {
		const Schema& schema = m_schema->getIndexSchema(i);
		fs::path path = segDir / ("index-" + schema.m_name);
		m_indices[i]->save(path.string());
	}
}

llong ReadableSegment::totalIndexSize() const {
	llong size = 0;
	for (size_t i = 0; i < m_indices.size(); ++i) {
		size += m_indices[i]->indexStorageSize();
	}
	return size;
}

void ReadableSegment::load(PathRef segDir) {
	assert(!segDir.empty());
	this->loadIsDel(segDir);
	this->openIndices(segDir);
	this->loadRecordStore(segDir);
}

void ReadableSegment::save(PathRef segDir) const {
	assert(!segDir.empty());
	if (m_tobeDel) {
		return; // not needed
	}
	this->saveRecordStore(segDir);
	this->saveIndices(segDir);
	this->saveIsDel(segDir);
}

size_t ReadableSegment::getPhysicRows() const {
	if (m_isPurged.size())
		return m_isPurged.max_rank0();
	else
		return m_isDel.size();
}

// logic id is immutable
// inline
size_t ReadableSegment::getPhysicId(size_t logicId) const {
	if (m_isPurged.empty()) {
		return logicId;
	} else {
		assert(this->getReadonlySegment() != NULL);
		assert(m_isPurged.size() == m_isDel.size());
		assert(logicId < m_isDel.size());
		return m_isPurged.rank0(logicId);
	}
}

size_t ReadableSegment::getLogicId(size_t physicId) const {
	if (m_isPurged.empty()) {
		return physicId;
	}
	else {
		assert(this->getReadonlySegment() != NULL);
		assert(m_isPurged.size() == m_isDel.size());
		assert(physicId < m_isPurged.max_rank0());
		return m_isPurged.select0(physicId);
	}
}

void ReadableSegment::addtoUpdateList(size_t logicId) {
	assert(m_isFreezed);
	if (!m_bookUpdates) {
		return;
	}
	const size_t rows = m_isDel.size();
	if ((m_updateList.unused() > 0 || m_updateList.size() < rows / 256) && m_updateBits.empty()) {
		m_updateList.push_back(logicId);
	}
	else if (!m_updateBits.empty()) {
		assert(m_updateBits.size() == rows + 1);
		m_updateBits.set1(logicId);
	}
	else {
		// reserve an extra bit as the guard
		m_updateBits.resize(rows + 1, false);
		bm_uint_t* bits = m_updateBits.bldata();
		uint32_t* idvec = m_updateList.data();
		size_t    idnum = m_updateList.size();
		for(size_t i = 0; i < idnum; ++i) {
			size_t id = idvec[i];
			assert(id < rows);
			terark_bit_set1(bits, id);
		}
		terark_bit_set1(bits, logicId);
		// set the last bit to 1 as the guard
		terark_bit_set1(bits, rows);
		m_updateList.clear();
	}
}

///////////////////////////////////////////////////////////////////////////////

ReadonlySegment::ReadonlySegment() {
	m_dataMemSize = 0;
	m_totalStorageSize = 0;
	m_dataInflateSize = 0;
	m_isFreezed = true;
	m_isPurgedMmap = 0;
}
ReadonlySegment::~ReadonlySegment() {
	if (m_isPurgedMmap) {
		mmap_close(m_isPurgedMmap, m_isPurged.mem_size());
		m_isPurged.risk_release_ownership();
	}
	m_colgroups.clear();
}

ReadonlySegment* ReadonlySegment::getReadonlySegment() const {
	return const_cast<ReadonlySegment*>(this);
}

llong ReadonlySegment::dataInflateSize() const {
	return m_dataMemSize;
}
llong ReadonlySegment::dataStorageSize() const {
	return m_dataMemSize;
}
llong ReadonlySegment::totalStorageSize() const {
	return m_totalStorageSize;
}

void ReadonlySegment::getValueAppend(llong id, valvec<byte>* val, DbContext* txn) const {
	assert(txn != nullptr);
	llong rows = m_isDel.size();
	if (id < 0 || id >= rows) {
		THROW_STD(out_of_range, "invalid id=%lld, rows=%lld", id, rows);
	}
	getValueByLogicId(id, val, txn);
}

void
ReadonlySegment::getValueByLogicId(size_t id, valvec<byte>* val, DbContext* ctx)
const {
	getValueByPhysicId(getPhysicId(id), val, ctx);
}

void
ReadonlySegment::getValueByPhysicId(size_t id, valvec<byte>* val, DbContext* ctx)
const {
	val->risk_set_size(0);
	ctx->buf1.risk_set_size(0);
	ctx->cols1.erase_all();

	// getValueAppend to ctx->buf1
	const size_t colgroupNum = m_colgroups.size();
	for (size_t i = 0; i < colgroupNum; ++i) {
		const Schema& iSchema = m_schema->getColgroupSchema(i);
		if (iSchema.m_keepCols.has_any1()) {
			size_t oldsize = ctx->buf1.size();
			m_colgroups[i]->getValueAppend(id, &ctx->buf1, ctx);
			iSchema.parseRowAppend(ctx->buf1, oldsize, &ctx->cols1);
		}
		else {
			ctx->cols1.grow(iSchema.columnNum());
		}
	}
	assert(ctx->cols1.size() == m_schema->m_colgroupSchemaSet->m_flattenColumnNum);

	// combine columns to ctx->cols2
	size_t baseColumnId = 0;
	ctx->cols2.m_base = ctx->cols1.m_base;
	ctx->cols2.m_cols.resize_fill(m_schema->columnNum());
	for (size_t i = 0; i < colgroupNum; ++i) {
		const Schema& iSchema = m_schema->getColgroupSchema(i);
		for (size_t j = 0; j < iSchema.columnNum(); ++j) {
			if (iSchema.m_keepCols[j]) {
				size_t parentColId = iSchema.parentColumnId(j);
				ctx->cols2.m_cols[parentColId] = ctx->cols1.m_cols[baseColumnId + j];
			}
		}
		baseColumnId += iSchema.columnNum();
	}

#if !defined(NDEBUG)
	for (size_t i = 0; i < ctx->cols2.size(); ++i) {
		assert(ctx->cols2.m_cols[i].isValid());
	}
#endif

	// combine to val
	m_schema->m_rowSchema->combineRow(ctx->cols2, val);
}

void
ReadonlySegment::indexSearchExactAppend(size_t mySegIdx, size_t indexId,
										fstring key, valvec<llong>* recIdvec,
										DbContext* ctx) const {
	size_t oldsize = recIdvec->size();
	auto index = m_indices[indexId].get();
	index->searchExactAppend(key, recIdvec, ctx);
	size_t newsize = oldsize;
	llong* recIdvecData = recIdvec->data();
	if (m_isPurged.empty()) {
		for (size_t k = oldsize; k < recIdvec->size(); ++k) {
			llong logicId = recIdvecData[k];
			if (!m_isDel[logicId])
				recIdvecData[newsize++] = logicId;
		}
	}
	else {
		assert(m_isPurged.size() == m_isDel.size());
		assert(this->getReadonlySegment() != NULL);
		for(size_t k = oldsize; k < recIdvec->size(); ++k) {
			size_t physicId = (size_t)recIdvecData[k];
			assert(physicId < m_isPurged.max_rank0());
			size_t logicId = m_isPurged.select0(physicId);
			if (!m_isDel[logicId])
				recIdvecData[newsize++] = logicId;
		}
	}
	recIdvec->risk_set_size(newsize);
}

void
ReadonlySegment::selectColumns(llong recId,
							   const size_t* colsId, size_t colsNum,
							   valvec<byte>* colsData, DbContext* ctx)
const {
	assert(recId >= 0);
	recId = getPhysicId(size_t(recId));
	colsData->erase_all();
	ctx->buf1.erase_all();
	ctx->offsets.resize_fill(m_colgroups.size(), UINT32_MAX);
	auto offsets = ctx->offsets.data();
	for(size_t i = 0; i < colsNum; ++i) {
		assert(colsId[i] < m_schema->m_rowSchema->columnNum());
		auto cp = m_schema->m_colproject[colsId[i]];
		size_t colgroupId = cp.colgroupId;
		size_t oldsize = ctx->buf1.size();
		const Schema& schema = m_schema->getColgroupSchema(colgroupId);
		if (offsets[colgroupId] == UINT32_MAX) {
			offsets[colgroupId] = ctx->cols1.size();
			m_colgroups[colgroupId]->getValueAppend(recId, &ctx->buf1, ctx);
			schema.parseRowAppend(ctx->buf1, oldsize, &ctx->cols1);
		}
		fstring d = ctx->cols1[offsets[colgroupId] + cp.subColumnId];
		if (i < colsNum-1)
			schema.projectToNorm(d, cp.subColumnId, colsData);
		else
			schema.projectToLast(d, cp.subColumnId, colsData);
	}
}

void
ReadonlySegment::selectOneColumn(llong recId, size_t columnId,
								 valvec<byte>* colsData, DbContext* ctx)
const {
	assert(recId >= 0);
	recId = getPhysicId(size_t(recId));
	assert(columnId < m_schema->m_rowSchema->columnNum());
	auto cp = m_schema->m_colproject[columnId];
	size_t colgroupId = cp.colgroupId;
	const Schema& schema = m_schema->getColgroupSchema(colgroupId);
//	printf("colprojects = %zd, colgroupId = %zd, schema.cols = %zd\n"
//		, m_schema->m_colproject.size(), colgroupId, schema.columnNum());
	if (schema.columnNum() == 1) {
		m_colgroups[colgroupId]->getValue(recId, colsData, ctx);
	}
	else {
		m_colgroups[colgroupId]->getValue(recId, &ctx->buf1, ctx);
		schema.parseRow(ctx->buf1, &ctx->cols1);
		colsData->erase_all();
		colsData->append(ctx->cols1[cp.subColumnId]);
	}
}

void ReadonlySegment::selectColgroups(llong recId,
						const size_t* cgIdvec, size_t cgIdvecSize,
						valvec<byte>* cgDataVec, DbContext* ctx) const {
	for(size_t i = 0; i < cgIdvecSize; ++i) {
		size_t cgId = cgIdvec[i];
		if (cgId >= m_schema->getColgroupNum()) {
			THROW_STD(out_of_range, "cgId = %zd, cgNum = %zd"
				, cgId, m_schema->getColgroupNum());
		}
		llong physicId = this->getPhysicId(recId);
		m_colgroups[cgId]->getValue(physicId, &cgDataVec[i], ctx);
	}
}

class ReadonlySegment::MyStoreIterForward : public StoreIterator {
	llong  m_id = 0;
	DbContextPtr m_ctx;
public:
	MyStoreIterForward(const ReadonlySegment* owner, DbContext* ctx)
	  : m_ctx(ctx) {
		m_store.reset(const_cast<ReadonlySegment*>(owner));
	}
	bool increment(llong* id, valvec<byte>* val) override {
		auto owner = static_cast<const ReadonlySegment*>(m_store.get());
		while (size_t(m_id) < owner->m_isDel.size() && owner->m_isDel[m_id])
			m_id++;
		if (size_t(m_id) < owner->m_isDel.size()) {
			*id = m_id++;
			owner->getValueByLogicId(*id, val, m_ctx.get());
			return true;
		}
		return false;
	}
	bool seekExact(llong id, valvec<byte>* val) override {
		m_id = id;
		llong id2 = -1;
		return increment(&id2, val);
	}
	void reset() override {
		m_id = 0;
	}
};
class ReadonlySegment::MyStoreIterBackward : public StoreIterator {
	llong  m_id;
	DbContextPtr m_ctx;
public:
	MyStoreIterBackward(const ReadonlySegment* owner, const DbContextPtr& ctx)
	  : m_ctx(ctx) {
		m_store.reset(const_cast<ReadonlySegment*>(owner));
		m_id = owner->m_isDel.size();
	}
	bool increment(llong* id, valvec<byte>* val) override {
		auto owner = static_cast<const ReadonlySegment*>(m_store.get());
		while (m_id > 0 && owner->m_isDel[m_id-1])
			 --m_id;
		if (m_id > 0) {
			*id = --m_id;
			owner->getValueByLogicId(*id, val, m_ctx.get());
			return true;
		}
		return false;
	}
	bool seekExact(llong id, valvec<byte>* val) override {
		m_id = id;
		llong id2 = -1;
		return increment(&id2, val);
	}
	void reset() override {
		auto owner = static_cast<const ReadonlySegment*>(m_store.get());
		m_id = owner->m_isDel.size();
	}
};
StoreIterator* ReadonlySegment::createStoreIterForward(DbContext* ctx) const {
	return new MyStoreIterForward(this, ctx);
}
StoreIterator* ReadonlySegment::createStoreIterBackward(DbContext* ctx) const {
	return new MyStoreIterBackward(this, ctx);
}

namespace {
	class TempFileList {
		const SchemaSet& m_schemaSet;
		valvec<byte> m_projRowBuf;
		valvec<ReadableStorePtr> m_readers;
		valvec<AppendableStore*> m_appenders;
		TERARK_IF_DEBUG(ColumnVec m_debugCols;,;);
	public:
		TempFileList(PathRef segDir, const SchemaSet& schemaSet)
			: m_schemaSet(schemaSet)
		{
			size_t cgNum = schemaSet.m_nested.end_i();
			m_readers.resize(cgNum);
			m_appenders.resize(cgNum);
			for (size_t i = 0; i < cgNum; ++i) {
				const Schema& schema = *schemaSet.m_nested.elem_at(i);
				if (schema.getFixedRowLen()) {
					m_readers[i] = new FixedLenStore(segDir, schema);
				}
				else {
					m_readers[i] = new SeqReadAppendonlyStore(segDir, schema);
				}
				m_appenders[i] = m_readers[i]->getAppendableStore();
			}
		}
		void writeColgroups(const ColumnVec& columns) {
			size_t colgroupNum = m_readers.size();
			for (size_t i = 0; i < colgroupNum; ++i) {
				const Schema& schema = *m_schemaSet.m_nested.elem_at(i);
				schema.selectParent(columns, &m_projRowBuf);
#if !defined(NDEBUG)
				schema.parseRow(m_projRowBuf, &m_debugCols);
				assert(m_debugCols.size() == schema.columnNum());
				for(size_t j = 0; j < m_debugCols.size(); ++j) {
					size_t k = schema.parentColumnId(j);
					assert(k < columns.size());
					assert(m_debugCols[j] == columns[k]);
				}
#endif
				m_appenders[i]->append(m_projRowBuf, NULL);
			}
		}
		void completeWrite() {
			size_t colgroupNum = m_readers.size();
			for (size_t i = 0; i < colgroupNum; ++i) {
				m_appenders[i]->shrinkToFit();
			}
		}
		ReadableStore* getStore(size_t cgId) const {
			return m_readers[cgId].get();
		}
		size_t size() const { return m_readers.size(); }
		size_t
		collectData(size_t cgId, StoreIterator* iter, SortableStrVec& strVec,
					size_t maxMemSize = size_t(-1)) const {
			assert(strVec.m_index.size() == 0);
			assert(strVec.m_strpool.size() == 0);
			const Schema& schema = *m_schemaSet.getSchema(cgId);
			const llong   rows = iter->getStore()->numDataRows();
			const size_t  fixlen = schema.getFixedRowLen();
			if (fixlen == 0) {
				valvec<byte> buf;
				llong  recId = INT_MAX; // for fail fast
				while (strVec.mem_size() < maxMemSize && iter->increment(&recId, &buf)) {
					assert(recId < rows);
					strVec.push_back(buf);
				}
				return strVec.size();
			}
			else { // ignore maxMemSize
				size_t size = fixlen * rows;
				strVec.m_strpool.resize_no_init(size);
				byte_t* basePtr = iter->getStore()->getRecordsBasePtr();
				memcpy(strVec.m_strpool.data(), basePtr, size);
				return rows;
			}
		}
	};
}

///@param iter record id from iter is physical id
///@param isDel new logical deletion mark
///@param isPurged physical deletion mark
///@note  physical deleted records must also be logical deleted
ReadableStore*
ReadonlySegment::buildDictZipStore(const Schema&, PathRef, StoreIterator& iter,
								   const bm_uint_t* isDel, const febitvec* isPurged) const {
	THROW_STD(invalid_argument,
		"Not Implemented, Only Implemented by DfaDbReadonlySegment");
}

/*
namespace {

// needs select0, it is slow
class PurgeMappingReadableStoreIterForward : public StoreIterator {
	StoreIteratorPtr m_iter;
	const rank_select_se* m_purgeBits;
public:
	PurgeMappingReadableStoreIterForward(const StoreIteratorPtr& iter, const rank_select_se& purgeBits) {
		assert(!purgeBits.empty());
		m_iter = iter;
		m_store = iter->m_store;
		m_purgeBits = &purgeBits;
	}
	bool increment(llong* id, valvec<byte>* val) override {
		while (m_iter->increment(id, val)) {
			if (!m_purgeBits->is1(size_t(*id))) {
				*id = m_purgeBits->select0(size_t(*id));
				return true;
			}
		}
		return false;
	}
	bool seekExact(llong id, valvec<byte>* val) override {
		if (m_iter->seekExact(id, val)) {
			if (!m_purgeBits->is1(size_t(*id))) {
				*id = m_purgeBits->select0(size_t(*id));
				return true;
			}
		}
		return false;
	}
	void reset() { m_iter->reset(); }
};

}
*/

void
ReadonlySegment::convFrom(CompositeTable* tab, size_t segIdx)
{
	auto tmpDir = m_segDir + ".tmp";
	fs::create_directories(tmpDir);

	DbContextPtr ctx;
	ReadableSegmentPtr input;
	{
		MyRwLock lock(tab->m_rwMutex, false);
		ctx.reset(tab->createDbContextNoLock());
		input = tab->m_segments[segIdx];
	}
	assert(input->getWritableStore() != nullptr);
	assert(input->m_isFreezed);
	assert(input->m_updateList.empty());
	assert(input->m_bookUpdates == false);
	input->m_updateList.reserve(1024);
	input->m_bookUpdates = true;
	m_isDel = input->m_isDel; // make a copy, input->m_isDel[*] may be changed
//	m_delcnt = m_isDel.popcnt(); // recompute delcnt
	llong logicRowNum = input->m_isDel.size();
	llong newRowNum = 0;
	assert(logicRowNum > 0);
	size_t indexNum = m_schema->getIndexNum();
{
	TempFileList colgroupTempFiles(tmpDir, *m_schema->m_colgroupSchemaSet);
{
	ColumnVec columns(m_schema->columnNum(), valvec_reserve());
	valvec<byte> buf;
	StoreIteratorPtr iter(input->createStoreIterForward(ctx.get()));
	llong prevId = -1;
	llong id = -1;
	while (iter->increment(&id, &buf) && id < logicRowNum) {
		assert(id >= 0);
		assert(id < logicRowNum);
		assert(prevId < id);
		if (!m_isDel[id]) {
			m_schema->m_rowSchema->parseRow(buf, &columns);
			colgroupTempFiles.writeColgroups(columns);
			newRowNum++;
			m_isDel.beg_end_set1(prevId+1, id);
			prevId = id;
		}
	}
	llong inputRowNum = id + 1;
	assert(inputRowNum <= logicRowNum);
	if (inputRowNum < logicRowNum) {
		fprintf(stderr
			, "WARN: inputRows[real=%lld saved=%lld], some data have lost\n"
			, inputRowNum, logicRowNum);
		input->m_isDel.beg_end_set1(inputRowNum, logicRowNum);
		this->m_isDel.beg_end_set1(inputRowNum, logicRowNum);
	}
	m_delcnt = m_isDel.popcnt(); // recompute delcnt
	assert(newRowNum <= inputRowNum);
	assert(size_t(logicRowNum - newRowNum) == m_delcnt);
}
	// build index from temporary index files
	colgroupTempFiles.completeWrite();
	m_indices.resize(indexNum);
	m_colgroups.resize(m_schema->getColgroupNum());
	for (size_t i = 0; i < indexNum; ++i) {
		SortableStrVec strVec;
		const Schema& schema = m_schema->getIndexSchema(i);
		auto tmpStore = colgroupTempFiles.getStore(i);
		StoreIteratorPtr iter = tmpStore->ensureStoreIterForward(NULL);
		colgroupTempFiles.collectData(i, iter.get(), strVec);
		m_indices[i] = this->buildIndex(schema, strVec);
		m_colgroups[i] = m_indices[i]->getReadableStore();
		if (!schema.m_enableLinearScan) {
			iter.reset();
			tmpStore->deleteFiles();
		}
	}
	for (size_t i = indexNum; i < colgroupTempFiles.size(); ++i) {
		const Schema& schema = m_schema->getColgroupSchema(i);
		auto tmpStore = colgroupTempFiles.getStore(i);
		if (schema.should_use_FixedLenStore()) {
			m_colgroups[i] = tmpStore;
			continue;
		}
		// dictZipLocalMatch is true by default
		// dictZipLocalMatch == false is just for experiment
		// dictZipLocalMatch should always be true in production
		// dictZipSampleRatio < 0 indicate don't use dictZip
		if (schema.m_dictZipLocalMatch && schema.m_dictZipSampleRatio >= 0.0) {
			double sRatio = schema.m_dictZipSampleRatio;
			double avgLen = double(tmpStore->dataInflateSize()) / newRowNum;
			if (sRatio > 0 || (sRatio < FLT_EPSILON && avgLen > 100)) {
				StoreIteratorPtr iter = tmpStore->ensureStoreIterForward(NULL);
				m_colgroups[i] = buildDictZipStore(schema, tmpDir, *iter, NULL, NULL);
				iter.reset();
				tmpStore->deleteFiles();
				continue;
			}
		}
		size_t maxMem = m_schema->m_compressingWorkMemSize;
		llong rows = 0;
		valvec<ReadableStorePtr> parts;
		StoreIteratorPtr iter = tmpStore->ensureStoreIterForward(NULL);
		while (rows < newRowNum) {
			SortableStrVec strVec;
			rows += colgroupTempFiles.collectData(i, iter.get(), strVec, maxMem);
			parts.push_back(this->buildStore(schema, strVec));
		}
		m_colgroups[i] = parts.size()==1 ? parts[0] : new MultiPartStore(parts);
		iter.reset();
		tmpStore->deleteFiles();
	}
}
	completeAndReload(tab, segIdx, &*input);

	fs::rename(tmpDir, m_segDir);
	input->deleteSegment();
}

void
ReadonlySegment::completeAndReload(CompositeTable* tab, size_t segIdx,
								   ReadableSegment* input) {
	m_dataMemSize = 0;
	m_dataInflateSize = 0;
	for (size_t i = 0; i < m_colgroups.size(); ++i) {
		m_dataMemSize += m_colgroups[i]->dataStorageSize();
		m_dataInflateSize += m_colgroups[i]->dataInflateSize();
	}

	if (this->m_delcnt) {
		m_isPurged.assign(m_isDel);
		m_isPurged.build_cache(true, false); // need select0
		m_withPurgeBits = true;
	}
	auto tmpDir = m_segDir + ".tmp";
	this->save(tmpDir);

	// reload as mmap
	m_isDel.clear();
	m_isPurged.clear();
	m_indices.erase_all();
	m_colgroups.erase_all();
	this->load(tmpDir);
	assert(this->m_isDel.size() == input->m_isDel.size());
	assert(this->m_isDel.popcnt() == this->m_delcnt);
	assert(this->m_isPurged.max_rank1() == this->m_delcnt);

	valvec<uint32_t> updateList;
	febitvec         updateBits;
	auto syncNewDeletionMark = [&]() {
		assert(input->m_bookUpdates);
		{
			SpinRwLock inputLock(input->m_segMutex, true);
			updateList.swap(input->m_updateList);
			updateBits.swap(input->m_updateBits);
		}
		if (updateList.size() > 0) {
			// this is the likely branch when lock(tab->m_rwMutex)
			assert(updateBits.size() == 0);
			std::sort(updateList.begin(), updateList.end());
			updateList.trim(
				std::unique(updateList.begin(),
							updateList.end())
			);
			auto dlist = updateList.data();
			auto isDel = this->m_isDel.bldata();
			size_t dlistSize = updateList.size();
			for (size_t i = 0; i < dlistSize; ++i) {
				assert(dlist[i] < m_isDel.size());
				size_t logicId = dlist[i];
				if (input->m_isDel[logicId])
					terark_bit_set1(isDel, logicId);
				else
					this->syncUpdateRecordNoLock(0, logicId, input);
			}
		}
		else if (updateBits.size() > 0) {
			assert(updateBits.size() == m_isDel.size()+1);
			size_t logicId = updateBits.zero_seq_len(0);
			while (logicId < m_isDel.size()) {
				if (!input->m_isDel[logicId]) {
					this->syncUpdateRecordNoLock(0, logicId, input);
				}
				logicId += 1 + updateBits.zero_seq_len(logicId + 1);
			}
			m_isDel.risk_memcpy(input->m_isDel);
		}
		else {
			// have nothing to update
			assert(updateList.size() == 0); // for set break point
		}
		// m_updateBits and m_updateList is safe to change in reader lock here
		updateBits.erase_all();
		updateList.erase_all();
	};
	syncNewDeletionMark(); // no lock
	MyRwLock lock(tab->m_rwMutex, false);
	assert(tab->m_segments[segIdx].get() == input);
	syncNewDeletionMark(); // reader locked
	lock.upgrade_to_writer();
	syncNewDeletionMark(); // writer locked
	m_delcnt = input->m_delcnt;
#if !defined(NDEBUG)
	{
		size_t computed_delcnt1 = this->m_isDel.popcnt();
		size_t computed_delcnt2 = input->m_isDel.popcnt();
		assert(computed_delcnt1 == input->m_delcnt);
		assert(computed_delcnt2 == input->m_delcnt);
	}
	valvec<byte> r1, r2;
	DbContextPtr ctx = tab->createDbContextNoLock();
	const Schema& rowSchema = *m_schema->m_rowSchema;
	for(size_t i = 0, rows = m_isDel.size(); i < rows; ++i) {
		if (!input->m_isDel[i]) {
			assert(!this->m_isDel[i]);
			this->getValue(i, &r1, ctx.get());
			input->getValue(i, &r2, ctx.get());
			int cmp = rowSchema.compareData(r1, r2);
			if (0 != cmp) {
				std::string js1 = rowSchema.toJsonStr(r1);
				std::string js2 = rowSchema.toJsonStr(r2);
				fprintf(stderr, "recId: %zd\n\tjs1[len=%zd]=%s\n\tjs2[len=%zd]=%s\n"
					, i, r1.size(), js1.c_str(), r2.size(), js2.c_str());
			}
			assert(0 == cmp);
			assert(m_isPurged.empty() || !m_isPurged[i]);
		} else {
			assert(this->m_isDel[i]);
		}
	}
	if (m_isPurged.size() > 0) {
		assert(m_isDel.size() == m_isPurged.size());
		for(size_t i = 0, rows = m_isDel.size(); i < rows; ++i) {
			if (m_isPurged[i]) {
				assert(m_isDel[i]);
				assert(input->m_isDel[i]);
			}
		}
	}
#endif
	assert(tab->m_segments[segIdx].get() == input);
	tab->m_segments[segIdx] = this;
	tab->m_segArrayUpdateSeq++;
}

// dstBaseId is for merge update
void
ReadonlySegment::syncUpdateRecordNoLock(size_t dstBaseId, size_t logicId,
										const ReadableSegment* input) {
	assert(input->m_isDel.is0(logicId));
	assert(this->m_isDel.is0(dstBaseId + logicId));
	auto dstPhysicId = this->getPhysicId(dstBaseId + logicId);
	auto srcPhysicId = input->getPhysicId(logicId);
	for (size_t colgroupId : m_schema->m_updatableColgroups) {
		auto&schema = m_schema->getColgroupSchema(colgroupId);
		auto dstColstore = this->m_colgroups[colgroupId].get();
		auto srcColstore = input->m_colgroups[colgroupId].get();
		assert(nullptr != dstColstore);
		assert(nullptr != srcColstore);
		auto fixlen = schema.getFixedRowLen();
		auto dstDataPtr = dstColstore->getRecordsBasePtr() + fixlen * dstPhysicId;
		auto srcDataPtr = srcColstore->getRecordsBasePtr() + fixlen * srcPhysicId;
		memcpy(dstDataPtr, srcDataPtr, fixlen);
	}
}

static inline
void pushRecord(SortableStrVec& strVec, const ReadableStore& store,
				llong physicId, size_t fixlen, DbContext* ctx) {
	size_t oldsize = strVec.str_size();
	store.getValueAppend(physicId, &strVec.m_strpool, ctx);
	if (!fixlen) {
		SortableStrVec::SEntry ent;
		ent.offset = oldsize;
		ent.length = strVec.str_size() - oldsize;
		ent.seq_id = uint32_t(strVec.m_index.size());
		strVec.m_index.push_back(ent);
	}
}
static
fs::path renameToBackupFromDir(PathRef segDir) {
	fs::path backupDir;
	for (int tmpNum = 0; ; ++tmpNum) {
		char szBuf[32];
		snprintf(szBuf, sizeof(szBuf), ".backup-%d", tmpNum);
		backupDir = segDir + szBuf;
		if (!fs::exists(backupDir))
			break;
		fprintf(stderr, "ERROR: existed %s\n", backupDir.string().c_str());
	}
	try { fs::rename(segDir, backupDir); }
	catch (const std::exception& ex) {
		std::string strDir = segDir.string();
		fprintf(stderr
			, "ERROR: rename(%s, %s.backup), ex.what = %s\n"
			, strDir.c_str(), strDir.c_str(), ex.what());
		abort();
	}
	return backupDir;
}

void
ReadonlySegment::purgeDeletedRecords(CompositeTable* tab, size_t segIdx) {
	DbContextPtr ctx(tab->createDbContext());
	ReadonlySegmentPtr input;
	{
		MyRwLock lock(tab->m_rwMutex, false);
		input = tab->m_segments[segIdx]->getReadonlySegment();
		assert(NULL != input);
		assert(!input->m_bookUpdates);
		input->m_updateList.reserve(1024);
		input->m_bookUpdates = true;
		lock.upgrade_to_writer();
		tab->m_purgeStatus = CompositeTable::PurgeStatus::purging;
	}
	fprintf(stderr, "INFO: purging %s\n", input->m_segDir.string().c_str());
	m_isDel = input->m_isDel; // make a copy, input->m_isDel[*] may be changed
	m_delcnt = m_isDel.popcnt(); // recompute delcnt
	m_indices.resize(m_schema->getIndexNum());
	m_colgroups.resize(m_schema->getColgroupNum());
	auto tmpSegDir = m_segDir + ".tmp";
	fs::create_directories(tmpSegDir);
	for (size_t i = 0; i < m_indices.size(); ++i) {
		m_indices[i] = purgeIndex(i, input.get(), ctx.get());
		m_colgroups[i] = m_indices[i]->getReadableStore();
	}
	for (size_t i = m_indices.size(); i < m_colgroups.size(); ++i) {
		m_colgroups[i] = purgeColgroup(i, input.get(), ctx.get(), tmpSegDir);
	}
	completeAndReload(tab, segIdx, &*input);
	assert(input->m_segDir == this->m_segDir);
	fs::path backupDir = renameToBackupFromDir(input->m_segDir);
	{
		fs::path backupDirCopy = backupDir;
		MyRwLock lock(tab->m_rwMutex, true);
		input->m_segDir.swap(backupDirCopy);
		input->deleteSegment(); // will delete backupDir
	}
	try { fs::rename(tmpSegDir, m_segDir); }
	catch (const std::exception& ex) {
		fs::rename(backupDir, m_segDir);
		std::string strDir = m_segDir.string();
		fprintf(stderr
			, "ERROR: rename(%s.tmp, %s), ex.what = %s\n"
			, strDir.c_str(), strDir.c_str(), ex.what());
		abort();
	}
}

ReadableIndexPtr
ReadonlySegment::purgeIndex(size_t indexId, ReadonlySegment* input, DbContext* ctx) {
	llong inputRowNum = input->m_isDel.size();
	assert(inputRowNum > 0);
	if (m_isDel.size() == m_delcnt) {
		return new EmptyIndexStore();
	}
	const bm_uint_t* isDel = m_isDel.bldata();
	SortableStrVec strVec;
	const Schema& schema = m_schema->getIndexSchema(indexId);
	const size_t  fixlen = schema.getFixedRowLen();
	if (0 == fixlen && schema.m_enableLinearScan) {
		ReadableStorePtr store = new SeqReadAppendonlyStore(input->m_segDir, schema);
		StoreIteratorPtr iter = store->createStoreIterForward(ctx);
		const bm_uint_t* purgeBits = input->m_isPurged.bldata();
		valvec<byte_t> rec;
		llong physicId = 0;
		for(llong logicId = 0; logicId < inputRowNum; ++logicId) {
			if (!purgeBits || !terark_bit_test(purgeBits, logicId)) {
				bool hasRow = iter->increment(&physicId, &rec);
				TERARK_RT_assert(hasRow, std::logic_error);
				TERARK_RT_assert(physicId <= logicId, std::logic_error);
				strVec.push_back(rec);
			}
		}
	}
	else
	{
		const auto& store = *input->m_indices[indexId]->getReadableStore();
		const bm_uint_t* purgeBits = input->m_isPurged.bldata();
		llong physicId = 0;
		for(llong logicId = 0; logicId < inputRowNum; ++logicId) {
			if (!purgeBits || !terark_bit_test(purgeBits, logicId)) {
				if (!terark_bit_test(isDel, logicId)) {
					pushRecord(strVec, store, physicId, fixlen, ctx);
				}
				physicId++;
			}
		}
	}
	return this->buildIndex(schema, strVec);
}

ReadableStorePtr
ReadonlySegment::purgeColgroup(size_t colgroupId, ReadonlySegment* input, DbContext* ctx, PathRef tmpSegDir) {
	assert(m_isDel.size() == input->m_isDel.size());
	if (m_isDel.size() == m_delcnt) {
		return new EmptyIndexStore();
	}
	const bm_uint_t* isDel = m_isDel.bldata();
	const llong inputRowNum = input->m_isDel.size();
	const Schema& schema = m_schema->getColgroupSchema(colgroupId);
	const auto& colgroup = *input->m_colgroups[colgroupId];
	if (schema.should_use_FixedLenStore()) {
		FixedLenStorePtr store = new FixedLenStore(tmpSegDir, schema);
		store->reserveRows(m_isDel.size() - m_delcnt);
		llong physicId = 0;
		const bm_uint_t* isPurged = input->m_isPurged.bldata();
		valvec<byte> buf;
		for (llong logicId = 0; logicId < inputRowNum; logicId++) {
			if (!isPurged || !terark_bit_test(isPurged, logicId)) {
				if (!terark_bit_test(isDel, logicId)) {
					colgroup.getValue(physicId, &buf, ctx);
					assert(buf.size() == schema.getFixedRowLen());
					store->append(buf, ctx);
				}
				physicId++;
			}
		}
		assert(!isPurged || llong(input->m_isPurged.max_rank0()) == physicId);
		return store;
	}
	if (schema.m_dictZipLocalMatch && schema.m_dictZipSampleRatio >= 0.0) {
		double sRatio = schema.m_dictZipSampleRatio;
		double avgLen = 1.0 * colgroup.dataInflateSize() / colgroup.numDataRows();
		if (sRatio > 0 || (sRatio < FLT_EPSILON && avgLen > 100)) {
			StoreIteratorPtr iter = colgroup.ensureStoreIterForward(ctx);
			return buildDictZipStore(schema, tmpSegDir, *iter, isDel, &input->m_isPurged);
		}
	}
	std::unique_ptr<SeqReadAppendonlyStore> seqStore;
	if (schema.m_enableLinearScan) {
		seqStore.reset(new SeqReadAppendonlyStore(tmpSegDir, schema));
	}
	SortableStrVec strVec;
	size_t fixlen = schema.getFixedRowLen();
	size_t maxMem = size_t(m_schema->m_compressingWorkMemSize);
	valvec<ReadableStorePtr> parts;
	auto partsPushRecord = [&](const ReadableStore& store, llong physicId) {
		if (terark_unlikely(strVec.mem_size() >= maxMem)) {
			parts.push_back(this->buildStore(schema, strVec));
			strVec.clear();
		}
		size_t oldsize = strVec.size();
		pushRecord(strVec, store, physicId, fixlen, ctx);
		if (seqStore)
			seqStore->append(fstring(strVec.m_strpool).substr(oldsize), NULL);
	};
	const bm_uint_t* oldpurgeBits = input->m_isPurged.bldata();
	assert(!oldpurgeBits || input->m_isPurged.size() == m_isDel.size());
	if (auto cgparts = dynamic_cast<const MultiPartStore*>(&colgroup)) {
		llong logicId = 0;
		for (size_t j = 0; j < cgparts->numParts(); ++j) {
			auto& partStore = *cgparts->getPart(j);
			llong partRows = partStore.numDataRows();
			llong subPhysicId = 0;
			while (logicId < inputRowNum && subPhysicId < partRows) {
				if (!oldpurgeBits || !terark_bit_test(oldpurgeBits, logicId)) {
					if (!terark_bit_test(isDel, logicId)) {
						partsPushRecord(partStore, subPhysicId);
					}
					subPhysicId++;
				}
				logicId++;
			}
			assert(subPhysicId == partRows);
		}
	}
	else {
		llong physicId = 0;
		for(llong logicId = 0; logicId < inputRowNum; ++logicId) {
			if (!oldpurgeBits || !terark_bit_test(oldpurgeBits, logicId)) {
				if (!terark_bit_test(isDel, logicId)) {
					partsPushRecord(colgroup, physicId);
				}
				physicId++;
			}
		}
#if !defined(NDEBUG)
		if (oldpurgeBits) { assert(size_t(physicId) == input->m_isPurged.max_rank0()); }
		else			  { assert(size_t(physicId) == m_isDel.size()); }
#endif
	}
	if (strVec.str_size() > 0) {
		parts.push_back(this->buildStore(schema, strVec));
	}
	return parts.size()==1 ? parts[0] : new MultiPartStore(parts);
}

void ReadonlySegment::load(PathRef segDir) {
	ReadableSegment::load(segDir);
	removePurgeBitsForCompactIdspace(segDir);
}

void ReadonlySegment::removePurgeBitsForCompactIdspace(PathRef segDir) {
//	assert(m_isDel.size() > 0);
	assert(m_isDelMmap != NULL);
	assert(m_isPurgedMmap == NULL);
	assert(m_isPurged.empty());
	PathRef purgeFpath = segDir / "IsPurged.rs";
	if (!fs::exists(purgeFpath)) {
		return;
	}
	fs::path formalFile = segDir / "IsDel";
	fs::path backupFile = segDir / "IsDel.backup";
	size_t isPurgedMmapBytes = 0;
	m_isPurgedMmap = (byte*)mmap_load(purgeFpath.string(), &isPurgedMmapBytes);
	m_isPurged.risk_mmap_from(m_isPurgedMmap, isPurgedMmapBytes);
	if (m_isDel.size() != m_isPurged.size()) {
		assert(m_isDel.size() < m_isPurged.size());
		// maybe last calling of this function was interupted
		if (fs::exists(backupFile)) {
			closeIsDel();
			fs::remove(formalFile);
			fs::rename(backupFile, formalFile);
			loadIsDel(segDir);
		}
	}
//	assert(m_withPurgeBits); // for self test debug
	if (m_withPurgeBits) {
		// logical record id will be m_isPurged.select0(physical id)
		return;
	}
	// delete IsPurged and compact bitmap m_isDel
	size_t oldRows = m_isDel.size();
	size_t newRows = m_isPurged.max_rank0();
	size_t newId = 0;
	febitvec newIsDel(newRows, false);
	for (size_t oldId = 0; oldId < oldRows; ++oldId) {
		if (!m_isPurged[oldId]) {
			if (m_isDel[oldId])
				newIsDel.set1(newId);
			++newId;
		}
		else {
			assert(m_isDel[oldId]);
		}
	}
	assert(newId == newRows);
	closeIsDel();
	fs::rename(formalFile, backupFile);
	m_isDel.swap(newIsDel);
	m_delcnt = m_isDel.popcnt();
	try {
		saveIsDel(segDir);
	}
	catch (const std::exception& ex) {
		fprintf(stderr, "ERROR: save %s failed: %s, restore backup\n"
			, formalFile.string().c_str(), ex.what());
		fs::rename(backupFile, formalFile);
		m_isDel.clear(); // by malloc, of newIsDel
		loadIsDel(segDir);
		return;
	}
	m_isDel.clear(); // by malloc, of newIsDel
	loadIsDel(segDir);
	mmap_close(m_isPurgedMmap, isPurgedMmapBytes);
	m_isPurgedMmap = NULL;
	m_isPurged.risk_release_ownership();
	fs::remove(purgeFpath);
	fs::remove(backupFile);
}

void ReadonlySegment::savePurgeBits(PathRef segDir) const {
	if (m_isPurgedMmap && segDir == m_segDir)
		return;
	if (m_isPurged.empty())
		return;
	assert(m_isPurged.size() == m_isDel.size());
	assert(m_isPurged.max_rank1() <= m_delcnt);
	PathRef purgeFpath = segDir / "IsPurged.rs";
	FileStream fp(purgeFpath.string().c_str(), "wb");
	fp.ensureWrite(m_isPurged.data(), m_isPurged.mem_size());
}

void ReadonlySegment::save(PathRef segDir) const {
	assert(!segDir.empty());
	if (m_tobeDel) {
		return;
	}
	savePurgeBits(segDir);
	ReadableSegment::save(segDir);
}

void ReadonlySegment::saveRecordStore(PathRef segDir) const {
	size_t indexNum = m_schema->getIndexNum();
	size_t colgroupNum = m_schema->getColgroupNum();
	for (size_t i = indexNum; i < colgroupNum; ++i) {
		const Schema& schema = m_schema->getColgroupSchema(i);
		fs::path fpath = segDir / ("colgroup-" + schema.m_name);
		m_colgroups[i]->save(fpath.string());
	}
}

void ReadonlySegment::loadRecordStore(PathRef segDir) {
	if (!m_colgroups.empty()) {
		THROW_STD(invalid_argument, "m_colgroups must be empty");
	}
	// indices must be loaded first
	assert(m_indices.size() == m_schema->getIndexNum());

	size_t indexNum = m_schema->getIndexNum();
	size_t colgroupNum = m_schema->getColgroupNum();
	m_colgroups.resize(colgroupNum);
	for (size_t i = 0; i < indexNum; ++i) {
		assert(m_indices[i]); // index must have be loaded
		auto store = m_indices[i]->getReadableStore();
		assert(nullptr != store);
		m_colgroups[i] = store;
	}
	SortableStrVec files;
	for(auto ent : fs::directory_iterator(segDir)) {
		std::string  fname = ent.path().filename().string();
		if (!fstring(fname).endsWith("-dict")) {
			files.push_back(fname);
		}
	}
	files.sort();
	for (size_t i = indexNum; i < colgroupNum; ++i) {
		const Schema& schema = m_schema->getColgroupSchema(i);
		std::string prefix = "colgroup-" + schema.m_name;
		size_t lo = files.lower_bound(prefix);
		if (lo >= files.size() || !files[lo].startsWith(prefix)) {
			THROW_STD(invalid_argument, "missing: %s",
				(segDir / prefix).string().c_str());
		}
		fstring fname = files[lo];
		if (fname.substr(prefix.size()).startsWith(".0000.")) {
			valvec<ReadableStorePtr> parts;
			size_t j = lo;
			while (j < files.size() && (fname = files[j]).startsWith(prefix)) {
				size_t partIdx = lcast(fname.substr(prefix.size()+1));
				assert(partIdx == j - lo);
				if (partIdx != j - lo) {
					THROW_STD(invalid_argument, "missing part: %s.%zd",
						(segDir / prefix).string().c_str(), j - lo);
				}
				parts.push_back(ReadableStore::openStore(schema, segDir, fname));
				++j;
			}
			m_colgroups[i] = new MultiPartStore(parts);
		}
		else {
			m_colgroups[i] = ReadableStore::openStore(schema, segDir, fname);
		}
	}
}

void ReadonlySegment::closeFiles() {
	if (m_isDelMmap) {
		size_t bitBytes = m_isDel.capacity()/8;
		mmap_close(m_isDelMmap, sizeof(uint64_t) + bitBytes);
		m_isDelMmap = nullptr;
		m_isDel.risk_release_ownership();
	}
	m_indices.clear();
	m_colgroups.clear();
}

ReadableIndex*
ReadonlySegment::openIndex(const Schema& schema, PathRef path) const {
	if (boost::filesystem::exists(path + ".zint")) {
		std::unique_ptr<ZipIntKeyIndex> store(new ZipIntKeyIndex(schema));
		store->load(path);
		return store.release();
	}
	if (boost::filesystem::exists(path + ".fixlen")) {
		std::unique_ptr<FixedLenKeyIndex> store(new FixedLenKeyIndex(schema));
		store->load(path);
		return store.release();
	}
	if (boost::filesystem::exists(path + ".empty")) {
		std::unique_ptr<EmptyIndexStore> store(new EmptyIndexStore());
		store->load(path);
		return store.release();
	}
	return nullptr;
}

ReadableIndex*
ReadonlySegment::buildIndex(const Schema& schema, SortableStrVec& indexData)
const {
	const size_t fixlen = schema.getFixedRowLen();
	if (schema.columnNum() == 1 && schema.getColumnMeta(0).isInteger()) {
		try {
			std::unique_ptr<ZipIntKeyIndex> index(new ZipIntKeyIndex(schema));
			index->build(schema.getColumnMeta(0).type, indexData);
			return index.release();
		}
		catch (const std::exception&) {
			// ignore and fall through
		}
	}
	if (fixlen && fixlen <= 16) {
		std::unique_ptr<FixedLenKeyIndex> index(new FixedLenKeyIndex(schema));
		index->build(schema, indexData);
		return index.release();
	}
	return nullptr; // derived class should override
}

ReadableStore*
ReadonlySegment::buildStore(const Schema& schema, SortableStrVec& storeData)
const {
	assert(!schema.should_use_FixedLenStore());
	if (schema.columnNum() == 1 && schema.getColumnMeta(0).isInteger()) {
		assert(schema.getFixedRowLen() > 0);
		try {
			std::unique_ptr<ZipIntStore> store(new ZipIntStore(schema));
			store->build(schema.getColumnMeta(0).type, storeData);
			return store.release();
		}
		catch (const std::exception&) {
			// ignore and fall through
			fprintf(stderr,
"try to build ZipIntStore: on %s failed, fallback to FixedLenStore\n",
				schema.m_name.c_str());
			std::unique_ptr<FixedLenStore> store(new FixedLenStore(m_segDir, schema));
			store->build(storeData);
			return store.release();
		}
	}
	return nullptr;
}

///////////////////////////////////////////////////////////////////////////////

DbTransaction::~DbTransaction() {
	assert(started != m_status);
}
DbTransaction::DbTransaction() {
	m_status = committed;
}
void DbTransaction::startTransaction() {
	assert(started != m_status);
	do_startTransaction();
	m_status = started;
}
bool DbTransaction::commit() {
	assert(started == m_status);
	if (do_commit()) {
		m_status = committed;
		return true;
	} else {
		m_status = rollbacked;
		return false;
	}
}
void DbTransaction::rollback() {
	assert(started == m_status);
	do_rollback();
	m_status = rollbacked;
}

WritableSegment::WritableSegment() {
}
WritableSegment::~WritableSegment() {
	if (!m_tobeDel)
		flushSegment();
}

void WritableSegment::pushIsDel(bool val) {
	const size_t ChunkBits = TERARK_IF_DEBUG(4*1024, 1*1024*1024);
	if (terark_unlikely(nullptr == m_isDelMmap)) {
		assert(m_isDel.size() == 0);
		assert(m_isDel.capacity() == 0);
		m_isDel.resize_fill(ChunkBits - 64, 0); // 64 is for uint64 header
		saveIsDel(m_segDir);
		m_isDel.clear();
		m_isDelMmap = loadIsDel_aux(m_segDir, m_isDel);
		((uint64_t*)m_isDelMmap)[0] = 0;
		m_isDel.risk_set_size(0);
		m_delcnt = 0;
	}
	else if (terark_unlikely(m_isDel.size() == m_isDel.capacity())) {
#if !defined(NDEBUG)
		assert((64 + m_isDel.size()) % ChunkBits == 0);
		size_t oldsize = m_isDel.size();
		size_t delcnt0 = m_isDel.popcnt();
		assert(delcnt0 == m_delcnt);
#endif
		size_t newCap = ((64+m_isDel.size()+2*ChunkBits-1) & ~(ChunkBits-1));
		closeIsDel();
		std::string fpath = (m_segDir / "IsDel").string();
		truncate_file(fpath, newCap/8);
		m_isDelMmap = loadIsDel_aux(m_segDir, m_isDel);
#if !defined(NDEBUG)
		size_t delcnt1 = m_isDel.popcnt();
		assert(nullptr != m_isDelMmap);
		assert(m_isDel.size() == oldsize);
		assert(delcnt1 == delcnt0);
		assert(delcnt1 == m_delcnt);
#endif
	}
	assert(m_isDel.size() < m_isDel.capacity());
	assert(m_isDel.size() == size_t(((uint64_t*)m_isDelMmap)[0]));
	m_isDel.unchecked_push_back(val);
	((uint64_t*)m_isDelMmap)[0] = m_isDel.size();
}

void WritableSegment::popIsDel() {
	assert(m_isDel.size() >= 1);
	assert(m_isDel.size() == size_t(((uint64_t*)m_isDelMmap)[0]));
	assert(nullptr != m_isDelMmap);
	m_isDel.pop_back();
	((uint64_t*)m_isDelMmap)[0] = m_isDel.size();
}

AppendableStore* WritableSegment::getAppendableStore() { return this; }
UpdatableStore* WritableSegment::getUpdatableStore() { return this; }
WritableStore* WritableSegment::getWritableStore() { return this; }

void
WritableSegment::getValueAppend(llong recId, valvec<byte>* val, DbContext* ctx)
const {
	assert(&ctx->buf1 != val);
	assert(&ctx->buf2 != val);
	if (m_schema->m_updatableColgroups.empty()) {
	//	m_wrtStore->getValueAppend(recId, val, ctx);
		this->getWrtStoreData(recId, val, ctx);
	}
	else {
		ctx->buf1.erase_all();
		ctx->cols1.erase_all();
	//	m_wrtStore->getValueAppend(recId, &ctx->buf1, ctx);
		this->getWrtStoreData(recId, &ctx->buf1, ctx);
		const size_t ProtectCnt = 100;
		if (m_isFreezed || m_isDel.unused() > ProtectCnt) {
			this->getCombineAppend(recId, val, ctx->buf1, ctx->cols1, ctx->cols2);
		}
		else {
			SpinRwLock  lock(m_segMutex, false);
			this->getCombineAppend(recId, val, ctx->buf1, ctx->cols1, ctx->cols2);
		}
	}
}

void
WritableSegment::indexSearchExactAppend(size_t mySegIdx, size_t indexId,
										fstring key, valvec<llong>* recIdvec,
										DbContext* ctx) const {
	assert(mySegIdx < ctx->m_segCtx.size());
	assert(ctx->getSegmentPtr(mySegIdx) == this);
	assert(m_isPurged.empty());
	IndexIterator* iter = ctx->getIndexIterNoLock(mySegIdx, indexId);
	llong recId = -1;
	int cmp = iter->seekLowerBound(key, &recId, &ctx->key2);
	if (cmp == 0) {
		// now IndexIterator::m_isUniqueInSchema is just for this quick check
		// faster than m_schema->getIndexSchema(indexId).m_isUnique
		// use lock if m_isDel.unused() is less than ProtectCnt
		const size_t ProtectCnt = 10;
		assert(iter->isUniqueInSchema() == m_schema->getIndexSchema(indexId).m_isUnique);
		if (iter->isUniqueInSchema()) {
			if (this->m_isFreezed || m_isDel.unused() >= ProtectCnt) {
				if (!m_isDel[recId])
					recIdvec->push_back(recId);
			}
			else {
				SpinRwLock lock(this->m_segMutex, false);
				if (!m_isDel[recId])
					recIdvec->push_back(recId);
			}
		}
		else {
			size_t oldsize = recIdvec->size();
			do {
				recIdvec->push_back(recId);
			} while (iter->increment(&recId, &ctx->key2) && key == ctx->key2);
			size_t i = oldsize, j = oldsize;
			size_t n = recIdvec->size();
			llong* p = recIdvec->data();
			if (this->m_isFreezed || m_isDel.unused() > ProtectCnt) {
				const bm_uint_t* isDel = m_isDel.bldata();
				for (; j < n; ++j) {
					intptr_t id = intptr_t(p[j]);
					if (!terark_bit_test(isDel, id))
						p[i++] = id;
				}
			}
			else { // same code, but with lock, lock as less as possible
				SpinRwLock lock(this->m_segMutex, false);
				const bm_uint_t* isDel = m_isDel.bldata();
				for (; j < n; ++j) {
					intptr_t id = intptr_t(p[j]);
					if (!terark_bit_test(isDel, id))
						p[i++] = id;
				}
			}
			recIdvec->risk_set_size(i);
		}
	}
	iter->reset();
}

void WritableSegment::getCombineAppend(llong recId, valvec<byte>* val,
					valvec<byte>& wrtBuf, ColumnVec& cols1, ColumnVec& cols2)
const {
	auto& sconf = *m_schema;
	assert(m_colgroups.size() == sconf.getColgroupNum());
	cols1.reserve(sconf.columnNum());
	sconf.m_wrtSchema->parseRowAppend(wrtBuf, 0, &cols1);
	for(size_t colgroupId : sconf.m_updatableColgroups) {
		auto& schema = sconf.getColgroupSchema(colgroupId);
		auto cg = m_colgroups[colgroupId].get();
		assert(nullptr != cg);
		size_t oldsize = wrtBuf.size();
		cg->getValueAppend(recId, &wrtBuf, NULL);
		schema.parseRowAppend(wrtBuf, oldsize, &cols1);
	}
	cols2.m_base = wrtBuf.data();
	cols2.m_cols.resize_fill(sconf.columnNum());
	auto  pCols1 = cols1.m_cols.data();
	auto  pCols2 = cols2.m_cols.data();
	auto& wrtSchema = *sconf.m_wrtSchema;
	for(size_t i  = 0, n = wrtSchema.columnNum(); i < n; ++i) {
		size_t j  = wrtSchema.parentColumnId(i);
		pCols2[j] = pCols1[i];
	}
	size_t baseColumnIdx1 = wrtSchema.columnNum();
	for(size_t colgroupId : sconf.m_updatableColgroups) {
		auto & schema = sconf.getColgroupSchema(colgroupId);
		size_t colnum = schema.columnNum();
		for(size_t i  = 0; i < colnum; ++i) {
			size_t j  = schema.parentColumnId(i);
			pCols2[j] = pCols1[baseColumnIdx1 + i];
		}
		baseColumnIdx1 += colnum;
	}
	sconf.m_rowSchema->combineRowAppend(cols2, val);
}

void WritableSegment::selectColumns(llong recId,
									const size_t* colsId, size_t colsNum,
									valvec<byte>* colsData, DbContext* ctx)
const {
	if (m_schema->m_updatableColgroups.empty()) {
		selectColumnsByWhole(recId, colsId, colsNum, colsData, ctx);
	}
	else {
		selectColumnsCombine(recId, colsId, colsNum, colsData, ctx);
	}
}

void WritableSegment::selectColumnsByWhole(llong recId,
									const size_t* colsId, size_t colsNum,
									valvec<byte>* colsData, DbContext* ctx)
const {
	assert(m_schema->m_updatableColgroups.empty());
	colsData->erase_all();
//	this->getValue(recId, &ctx->buf1, ctx);
	this->getWrtStoreData(recId, &ctx->buf1, ctx);
	const Schema& schema = *m_schema->m_rowSchema;
	schema.parseRow(ctx->buf1, &ctx->cols1);
	assert(ctx->cols1.size() == schema.columnNum());
	for(size_t i = 0; i < colsNum; ++i) {
		size_t columnId = colsId[i];
		assert(columnId < schema.columnNum());
		if (i < colsNum-1)
			schema.projectToNorm(ctx->cols1[columnId], columnId, colsData);
		else
			schema.projectToLast(ctx->cols1[columnId], columnId, colsData);
	}
}

void WritableSegment::selectColumnsCombine(llong recId,
									const size_t* colsIdvec, size_t colsNum,
									valvec<byte>* colsData, DbContext* ctx)
const {
	colsData->erase_all();
	const SchemaConfig& sconf = *m_schema;
	const Schema& rowSchema = *sconf.m_rowSchema;
	ctx->cols1.erase_all();
	for(size_t i = 0; i < colsNum; ++i) {
		size_t columnId = colsIdvec[i];
		assert(columnId < rowSchema.columnNum());
		auto colproj = sconf.m_colproject[columnId];
		auto schema = &sconf.getColgroupSchema(colproj.colgroupId);
		if (schema->m_isInplaceUpdatable) {
			assert(colproj.colgroupId >= sconf.getIndexNum());
			size_t fixlen = schema->getFixedRowLen();
			assert(fixlen > 0);
			auto store = m_colgroups[colproj.colgroupId].get();
			assert(nullptr != store);
			byte_t* basePtr = store->getRecordsBasePtr();
			assert(nullptr != basePtr);
			auto&  colmeta = schema->getColumnMeta(colproj.subColumnId);
			auto   coldata = basePtr + fixlen * recId + colmeta.fixedOffset;
			assert(colmeta.fixedLen > 0);
			assert(colmeta.fixedEndOffset() <= fixlen);
			colsData->append(coldata, colmeta.fixedLen);
		}
		else {
			schema = sconf.m_wrtSchema.get();
			if (ctx->cols1.empty()) {
			//	m_wrtStore->getValue(recId, &ctx->buf1, ctx);
				this->getWrtStoreData(recId, &ctx->buf1, ctx);
				schema->parseRow(ctx->buf1, &ctx->cols1);
			}
			size_t subColumnId = sconf.m_rowSchemaColToWrtCol[columnId];
			assert(subColumnId < sconf.m_wrtSchema->columnNum());
			fstring coldata = ctx->cols1[subColumnId];
			if (i < colsNum-1)
				rowSchema.projectToNorm(coldata, columnId, colsData);
			else
				rowSchema.projectToLast(coldata, columnId, colsData);
		}
	}
}

void WritableSegment::selectOneColumn(llong recId, size_t columnId,
									  valvec<byte>* colsData, DbContext* ctx)
const {
	assert(columnId < m_schema->columnNum());
	auto colproj = m_schema->m_colproject[columnId];
	auto& schema = m_schema->getColgroupSchema(colproj.colgroupId);
	if (schema.m_isInplaceUpdatable) {
		auto store = m_colgroups[colproj.colgroupId].get();
		auto fixlen = schema.getFixedRowLen();
		assert(nullptr != store);
		assert(fixlen > 0);
		const auto& colmeta = schema.getColumnMeta(colproj.subColumnId);
		const byte* basePtr = store->getRecordsBasePtr();
		const byte* coldata = basePtr + fixlen * recId + colmeta.fixedOffset;
		assert(colmeta.fixedLen > 0);
		assert(colmeta.fixedEndOffset() <= fixlen);
		colsData->assign(coldata, colmeta.fixedLen);
	}
	else {
		const Schema& wrtSchema = *m_schema->m_wrtSchema;
	//	m_wrtStore->getValue(recId, &ctx->buf1, ctx);
		this->getWrtStoreData(recId, &ctx->buf1, ctx);
		wrtSchema.parseRow(ctx->buf1, &ctx->cols1);
		assert(ctx->cols1.size() == wrtSchema.columnNum());
		colsData->erase_all();
		if (m_schema->m_updatableColgroups.empty()) {
			assert(m_schema->m_wrtSchema == m_schema->m_rowSchema);
			assert(m_schema->m_rowSchemaColToWrtCol.empty());
			wrtSchema.projectToLast(ctx->cols1[columnId], columnId, colsData);
		}
		else {
			size_t wrtColumnId = m_schema->m_rowSchemaColToWrtCol[columnId];
			assert(wrtColumnId < wrtSchema.columnNum());
			wrtSchema.projectToLast(ctx->cols1[wrtColumnId], columnId, colsData);
		}
	}
}

void WritableSegment::selectColgroups(llong recId,
						const size_t* cgIdvec, size_t cgIdvecSize,
						valvec<byte>* cgDataVec, DbContext* ctx) const {
	for(size_t i = 0; i < cgIdvecSize; ++i) {
		size_t cgId = cgIdvec[i];
		if (cgId >= m_schema->getColgroupNum()) {
			THROW_STD(out_of_range, "cgId = %zd, cgNum = %zd"
				, cgId, m_schema->getColgroupNum());
		}
		const ReadableStore* store = m_colgroups.empty() ? NULL : m_colgroups[cgId].get();
		if (store) {
			// inplace updatable store
			assert(store->getRecordsBasePtr() != NULL);
			store->getValue(recId, &cgDataVec[i], ctx);
		}
		else {
			const Schema& schema = m_schema->getColgroupSchema(cgId);
			const valvec<size_t>& colsIdvec = schema.getProj();
			selectColumns(recId, colsIdvec.data(), colsIdvec.size(),
						  &cgDataVec[i], ctx);
		}
	}
}

void WritableSegment::flushSegment() {
	if (m_tobeDel) {
		return;
	}
	if (m_isDirty) {
		save(m_segDir);
		m_isDirty = false;
	}
}

void WritableSegment::saveRecordStore(PathRef segDir) const {
	for (size_t colgroupId : m_schema->m_updatableColgroups) {
		const Schema& schema = m_schema->getColgroupSchema(colgroupId);
		assert(schema.m_isInplaceUpdatable);
		assert(schema.getFixedRowLen() > 0);
		auto store = m_colgroups[colgroupId];
		assert(nullptr != store);
		store->save(segDir / "colgroup-" + schema.m_name);
	}
	m_wrtStore->save(segDir / "__wrtStore__");
}

void WritableSegment::loadRecordStore(PathRef segDir) {
	assert(m_colgroups.size() == 0);
	m_colgroups.resize(m_schema->getColgroupNum());
	for (size_t colgroupId : m_schema->m_updatableColgroups) {
		const Schema& schema = m_schema->getColgroupSchema(colgroupId);
		assert(schema.m_isInplaceUpdatable);
		assert(schema.getFixedRowLen() > 0);
		std::unique_ptr<FixedLenStore> store(new FixedLenStore(segDir, schema));
	//	auto store(std::make_unique<FixedLenStore>(segDir, schema));
		store->openStore();
		m_colgroups[colgroupId] = store.release();
	}
	m_wrtStore->load(segDir / "__wrtStore__");
}

WritableSegment* WritableSegment::getWritableSegment() const {
	return const_cast<WritableSegment*>(this);
}

llong WritableSegment::totalStorageSize() const {
	llong size = m_wrtStore->dataStorageSize() + totalIndexSize();
	for (size_t colgroupId : m_schema->m_updatableColgroups) {
		auto store = m_colgroups[colgroupId].get();
		assert(nullptr != store);
		size += store->dataStorageSize();
	}
	return size;
}

llong WritableSegment::dataInflateSize() const {
	llong size = m_wrtStore->dataInflateSize();
	for (size_t colgroupId : m_schema->m_updatableColgroups) {
		auto store = m_colgroups[colgroupId].get();
		assert(nullptr != store);
		size += store->dataInflateSize();
	}
	return size;
}

llong WritableSegment::dataStorageSize() const {
	llong size = m_wrtStore->dataStorageSize();
	for (size_t colgroupId : m_schema->m_updatableColgroups) {
		auto store = m_colgroups[colgroupId].get();
		assert(nullptr != store);
		size += store->dataStorageSize();
	}
	return size;
}

class WritableSegment::MyStoreIter : public StoreIterator {
	const SchemaConfig& m_sconf;
	const WritableSegment* m_wrtSeg;
	StoreIteratorPtr m_wrtIter;
	valvec<byte> m_wrtBuf;
	ColumnVec    m_cols1;
	ColumnVec    m_cols2;
public:
	MyStoreIter(const WritableSegment* wrtSeg, StoreIterator* wrtIter,
				DbContext* ctx, const SchemaConfig& sconf)
	  : m_sconf(sconf)
	{
		m_store = const_cast<WritableSegment*>(wrtSeg);
		m_wrtSeg = wrtSeg;
		m_wrtIter = wrtIter;
	}
	bool increment(llong* id, valvec<byte>* val) override {
		// don't test m_isDel, it requires lock
		// inplace readable store also requires lock
		if (m_sconf.m_updatableColgroups.empty()) {
			if (m_wrtIter->increment(id, val)) {
				return true;
			}
			return false;
		}
		if (m_wrtIter->increment(id, &m_wrtBuf)) {
			val->erase_all();
			m_wrtSeg->getCombineAppend(*id, val, m_wrtBuf, m_cols1, m_cols2);
			return true;
		}
		return false;
	}
	bool seekExact(llong id, valvec<byte>* val) override {
		m_wrtIter->reset();
		if (m_sconf.m_updatableColgroups.empty()) {
			return m_wrtIter->seekExact(id, val);
		}
		m_wrtBuf.erase_all();
		m_cols1.erase_all();
		if (m_wrtIter->seekExact(id, &m_wrtBuf)) {
			val->erase_all();
			m_wrtSeg->getCombineAppend(id, val, m_wrtBuf, m_cols1, m_cols2);
			return true;
		}
		return false;
	}
	void reset() override {
		m_wrtIter->reset();
	}
};

StoreIterator*
WritableSegment::createStoreIterForward(DbContext* ctx) const {
	if (m_schema->m_updatableColgroups.empty()) {
		return m_wrtStore->createStoreIterForward(ctx);
	}
	else {
		auto wrtIter = m_wrtStore->createStoreIterForward(ctx);
		return new MyStoreIter(this, wrtIter, ctx, *m_schema);
	}
}

StoreIterator*
WritableSegment::createStoreIterBackward(DbContext* ctx) const {
	if (m_schema->m_updatableColgroups.empty()) {
		return m_wrtStore->createStoreIterBackward(ctx);
	}
	else {
		auto wrtIter = m_wrtStore->createStoreIterBackward(ctx);
		return new MyStoreIter(this, wrtIter, ctx, *m_schema);
	}
}

//static void splitRowToWrt

llong WritableSegment::append(fstring row, DbContext* ctx) {
	auto store = m_wrtStore->getAppendableStore();
	assert(nullptr != store);
	const SchemaConfig& sconf = *m_schema;
	if (sconf.m_updatableColgroups.empty()) {
		return store->append(row, ctx);
	}
	else {
		sconf.m_rowSchema->parseRow(row, &ctx->cols1);
		sconf.m_wrtSchema->selectParent(ctx->cols1, &ctx->buf1);
		llong id1 = store->append(ctx->buf1, ctx);
		for (size_t colgroupId : sconf.m_updatableColgroups) {
			store = m_colgroups[colgroupId]->getAppendableStore();
			assert(nullptr != store);
			const Schema& schema = sconf.getColgroupSchema(colgroupId);
			schema.selectParent(ctx->cols1, &ctx->buf1);
			llong id2 = store->append(ctx->buf1, ctx);
			TERARK_RT_assert(id1 == id2, std::logic_error);
		}
		return id1;
	}
}

void WritableSegment::update(llong id, fstring row, DbContext* ctx) {
	assert(id <= llong(m_isDel.size()));
	auto store = m_wrtStore->getUpdatableStore();
	assert(nullptr != store);
	const SchemaConfig& sconf = *m_schema;
	if (sconf.m_updatableColgroups.empty()) {
		store->update(id, row, ctx);
	}
	else {
		sconf.m_rowSchema->parseRow(row, &ctx->cols1);
		sconf.m_wrtSchema->selectParent(ctx->cols1, &ctx->buf1);
		store->update(id, ctx->buf1, ctx);
		for (size_t colgroupId : sconf.m_updatableColgroups) {
			store = m_colgroups[colgroupId]->getUpdatableStore();
			assert(nullptr != store);
			const Schema& schema = sconf.getColgroupSchema(colgroupId);
			schema.selectParent(ctx->cols1, &ctx->buf1);
			store->update(id, ctx->buf1, ctx);
		}
	}
}

void WritableSegment::remove(llong id, DbContext* ctx) {
	m_wrtStore->getWritableStore()->remove(id, ctx);
}

void WritableSegment::shrinkToFit() {
	for (size_t colgroupId : m_schema->m_updatableColgroups) {
		auto store = m_colgroups[colgroupId]->getAppendableStore();
		assert(nullptr != store);
		store->shrinkToFit();
	}
	m_wrtStore->getAppendableStore()->shrinkToFit();
}

void WritableSegment::getWrtStoreData(llong subId, valvec<byte>* buf, DbContext* ctx)
const {
	if (m_hasLockFreePointSearch) {
		m_wrtStore->getValue(subId, buf, ctx);
	}
	else {
		ctx->getWrSegWrtStoreData(this, subId, buf);
	}
}

///////////////////////////////////////////////////////////////////////////////

SmartWritableSegment::~SmartWritableSegment() {
}

void
SmartWritableSegment::getValueAppend(llong id, valvec<byte>* val, DbContext* txn)
const {
	assert(txn != nullptr);
	// m_indices also store index keys
//	DbContextPtr dummy;
	assert(0);
	// should similar to ReadonlySegment::getValueAppend(...)
}

class SmartWritableSegment::MyStoreIterForward : public StoreIterator {
	size_t m_id;
	DbContextPtr m_ctx;
public:
	MyStoreIterForward(const SmartWritableSegment* owner, DbContext* ctx) {
		m_store.reset(const_cast<SmartWritableSegment*>(owner));
		m_id = 0;
		m_ctx.reset(ctx);
	}
	bool increment(llong* id, valvec<byte>* val) override {
		auto owner = static_cast<const SmartWritableSegment*>(m_store.get());
		if (m_id < owner->m_isDel.size()) {
			*id = m_id;
			owner->getValue(m_id, val, m_ctx.get());
			m_id++;
			return true;
		}
		return false;
	}
	bool seekExact(llong id, valvec<byte>* val) override {
		auto owner = static_cast<const SmartWritableSegment*>(m_store.get());
		m_id = id;
		if (owner->m_isDel[id]) {
			return false;
		}
		owner->getValue(id, val, m_ctx.get());
		return true;
	}
	void reset() override {
		m_id = 0;
	}
};
class SmartWritableSegment::MyStoreIterBackward : public StoreIterator {
	size_t m_id;
	DbContextPtr m_ctx;
public:
	MyStoreIterBackward(const SmartWritableSegment* owner, DbContext* ctx) {
		m_store.reset(const_cast<SmartWritableSegment*>(owner));
		m_id = owner->m_isDel.size();
		m_ctx.reset(ctx);
	}
	bool increment(llong* id, valvec<byte>* val) override {
		auto owner = static_cast<const SmartWritableSegment*>(m_store.get());
		if (m_id > 0) {
			*id = --m_id;
			owner->getValue(m_id, val, &*m_ctx);
			return true;
		}
		return false;
	}
	bool seekExact(llong id, valvec<byte>* val) override {
		auto owner = static_cast<const SmartWritableSegment*>(m_store.get());
		m_id = id;
		if (owner->m_isDel[id]) {
			return false;
		}
		owner->getValue(id, val, m_ctx.get());
		return true;
	}
	void reset() override {
		auto owner = static_cast<const SmartWritableSegment*>(m_store.get());
		m_id = owner->m_isDel.size();
	}
};

StoreIterator* SmartWritableSegment::createStoreIterForward(DbContext* ctx) const {
	return new MyStoreIterForward(this, ctx);
}
StoreIterator* SmartWritableSegment::createStoreIterBackward(DbContext* ctx) const {
	return new MyStoreIterBackward(this, ctx);
}

void SmartWritableSegment::saveRecordStore(PathRef dir) const {
	abort();
}

void SmartWritableSegment::loadRecordStore(PathRef dir) {
	abort();
}

llong SmartWritableSegment::dataStorageSize() const {
	abort();
	return 0;
}

llong SmartWritableSegment::totalStorageSize() const {
	abort();
	return totalIndexSize() + 0;
}

} } // namespace terark::db
