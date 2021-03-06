#include "nlt_store.hpp"
#include <terark/int_vector.hpp>
#include <terark/fast_zip_blob_store.hpp>
#include <typeinfo>
#include <float.h>
#include <mutex>

namespace terark { namespace db { namespace dfadb {

TERARK_DB_REGISTER_STORE("nlt", NestLoudsTrieStore);

NestLoudsTrieStore::NestLoudsTrieStore(const Schema& schema) : m_schema(schema) {
}
NestLoudsTrieStore::~NestLoudsTrieStore() {
}

llong NestLoudsTrieStore::dataStorageSize() const {
	return m_store->mem_size();
}

llong NestLoudsTrieStore::dataInflateSize() const {
	return m_store->total_data_size();
}

llong NestLoudsTrieStore::numDataRows() const {
	return m_store->num_records();
}

void NestLoudsTrieStore::getValueAppend(llong id, valvec<byte>* val, DbContext* ctx) const {
	m_store->get_record_append(size_t(id), val);
}

StoreIterator* NestLoudsTrieStore::createStoreIterForward(DbContext*) const {
	return nullptr; // not needed
}

StoreIterator* NestLoudsTrieStore::createStoreIterBackward(DbContext*) const {
	return nullptr; // not needed
}

template<class Class>
static
Class* doBuild(const NestLoudsTrieConfig& conf,
			   const Schema& schema, SortableStrVec& strVec) {
	std::unique_ptr<Class> trie(new Class());
	trie->build_from(strVec, conf);
	return trie.release();
}

static
void initConfigFromSchema(NestLoudsTrieConfig& conf, const Schema& schema) {
	conf.initFromEnv();
	if (schema.m_sufarrMinFreq) {
		conf.saFragMinFreq = (byte_t)schema.m_sufarrMinFreq;
	}
	if (schema.m_minFragLen) {
		conf.minFragLen = schema.m_minFragLen;
	}
	if (schema.m_maxFragLen) {
		conf.maxFragLen = schema.m_maxFragLen;
	}
	if (schema.m_nltDelims.size()) {
		conf.setBestDelims(schema.m_nltDelims.c_str());
	}
	conf.nestLevel = schema.m_nltNestLevel;
}

static
BlobStore* nltBuild(const Schema& schema, SortableStrVec& strVec) {
	NestLoudsTrieConfig conf;
	initConfigFromSchema(conf, schema);
	switch (schema.m_rankSelectClass) {
	case -256:
		return doBuild<NestLoudsTrieBlobStore_IL>(conf, schema, strVec);
	case +256:
		return doBuild<NestLoudsTrieBlobStore_SE>(conf, schema, strVec);
	case +512:
		return doBuild<NestLoudsTrieBlobStore_SE_512>(conf, schema, strVec);
	default:
		fprintf(stderr, "WARN: invalid schema(%s).rs = %d, use default: se_512\n"
					  , schema.m_name.c_str(), schema.m_rankSelectClass);
		return doBuild<NestLoudsTrieBlobStore_SE_512>(conf, schema, strVec);
	}
}

void NestLoudsTrieStore::build(const Schema& schema, SortableStrVec& strVec) {
	if (schema.m_dictZipSampleRatio > 0) {
		std::unique_ptr<DictZipBlobStore> zds(new DictZipBlobStore());
		zds->build_none_local_match(strVec, schema.m_dictZipSampleRatio);
		m_store.reset(zds.release());
	}
	else if (schema.m_useFastZip) {
		std::unique_ptr<FastZipBlobStore> fzds(new FastZipBlobStore());
		NestLoudsTrieConfig  conf;
		initConfigFromSchema(conf, schema);
		fzds->build_from(strVec, conf);
		m_store.reset(fzds.release());
	}
	else {
		m_store.reset(nltBuild(schema, strVec));
	}
}

void
NestLoudsTrieStore::build_by_iter(const Schema& schema, PathRef fpath,
								  StoreIterator& iter,
								  const bm_uint_t* isDel,
								  const febitvec* isPurged) {
	TERARK_RT_assert(schema.m_dictZipSampleRatio >= 0, std::invalid_argument);
	std::unique_ptr<DictZipBlobStore> zds(new DictZipBlobStore());
	std::unique_ptr<DictZipBlobStore::ZipBuilder> builder(zds->createZipBuilder());
	double sampleRatio = schema.m_dictZipSampleRatio > FLT_EPSILON
					   ? schema.m_dictZipSampleRatio : 0.05;
	{
		TERARK_RT_assert(nullptr != iter.getStore(), std::invalid_argument);
		llong dataSize = iter.getStore()->dataInflateSize();
		if (dataSize * sampleRatio >= INT32_MAX * 0.95) {
			sampleRatio = INT32_MAX * 0.95 / dataSize;
		}
	}

	// 1. sample memory usage = inputBytes*sampleRatio, and will
	//    linear scan the input data
	// 2. builder->prepare() will build the suffix array and cache
	//    for suffix array, and this is all in-memery computing,
	//    the memory usage is about 5*inputBytes*sampleRatio, after
	//    `prepare` finished, the total memory usage is about
	//    6*inputBytes*sampleRatio
	// 3. builder->addRecord() will send the records into compressing
	//    pipeline, records will be compressed parallel, this will
	//    take a long time, the total memory during compressing is
	//    6*inputBytes*sampleRatio, plus few additional working memory
	// 4. using lock, the concurrent large memory using durations in
	//    multi threads are serialized, then the peak memory usage
	//    is reduced
	static std::mutex reduceMemMutex;
	// the lock will be hold for a long time, maybe several minutes
	std::unique_lock<std::mutex> lock(reduceMemMutex, std::defer_lock);

	valvec<byte> rec;
	auto emptyCheckProtect = [&](size_t sampled) {
	//	TERARK_RT_assert(sampled > 0, std::logic_error);
		if (0 == sampled) {
			if (rec.empty())
				builder->addSample("Hello World!"); // for fallback
			else
				builder->addSample(rec);
		}
	};	
	if (NULL == isPurged || isPurged->size() == 0) {
		llong recId;
		size_t sampled = 0;
		while (iter.increment(&recId, &rec)) {
			if (NULL == isDel || !terark_bit_test(isDel, recId)) {
				if (rand() < RAND_MAX * sampleRatio ) {
					builder->addSample(rec);
					sampled++;
				}
			}
		}
		emptyCheckProtect(sampled);
		lock.lock(); // start lock
		builder->prepare(recId + 1, fpath.string());
		iter.reset();
		while (iter.increment(&recId, &rec)) {
			if (NULL == isDel || !terark_bit_test(isDel, recId)) {
				builder->addRecord(rec);
			}
		}
	}
	else {
		assert(NULL != isDel);
		llong  newPhysicId = 0;
		llong  physicId = 0;
		size_t logicNum = isPurged->size();
		size_t sampled = 0;
		const bm_uint_t* isPurgedptr = isPurged->bldata();
		for (size_t logicId = 0; logicId < logicNum; ++logicId) {
			if (!terark_bit_test(isPurgedptr, logicId)) {
				if (!terark_bit_test(isDel, logicId)) {
					bool hasData = iter.seekExact(physicId, &rec);
					TERARK_RT_assert(hasData, std::logic_error);
				//	if (hasData && rec.empty()) {
				//		hasData = false;
				//	}
					if (rand() < RAND_MAX * sampleRatio) {
						builder->addSample(rec);
						sampled++;
					}
					newPhysicId++;
				}
				physicId++;
			}
		}
		emptyCheckProtect(sampled);
		lock.lock(); // start lock
		builder->prepare(newPhysicId, fpath.string());
		iter.reset();
		physicId = 0;
		for (size_t logicId = 0; logicId < logicNum; ++logicId) {
			if (!terark_bit_test(isPurgedptr, logicId)) {
				bool hasData = iter.increment(&physicId, &rec);
				TERARK_RT_assert(hasData, std::logic_error);
				if (!terark_bit_test(isDel, logicId)) {
					builder->addRecord(rec);
				}
				physicId++;
			}
		}
	}
	zds->completeBuild(*builder);
	builder.reset(); // explicit destory builder
	lock.unlock();   // explicit unlock
	m_store.reset(zds.release());
}

void NestLoudsTrieStore::load(PathRef path) {
	std::string fpath = fstring(path.string()).endsWith(".nlt")
					  ? path.string()
					  : path.string() + ".nlt";
	m_store.reset(BlobStore::load_from(fpath, m_schema.m_mmapPopulate));
}

void NestLoudsTrieStore::save(PathRef path) const {
	std::string fpath = fstring(path.string()).endsWith(".nlt")
						? path.string()
						: path.string() + ".nlt";
	if (BaseDFA* dfa = dynamic_cast<BaseDFA*>(&*m_store)) {
		dfa->save_mmap(fpath.c_str());
	}
	else if (auto zds = dynamic_cast<FastZipBlobStore*>(&*m_store)) {
		zds->save_mmap(fpath);
	}
	else if (auto zds = dynamic_cast<DictZipBlobStore*>(&*m_store)) {
		zds->save_mmap(fpath);
	}
	else {
		THROW_STD(invalid_argument, "Unexpected");
	}
}

}}} // namespace terark::db::dfadb
