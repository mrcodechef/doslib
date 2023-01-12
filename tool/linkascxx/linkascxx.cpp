
#include <sys/types.h>
#include <stdlib.h>
#include <stdarg.h>
#include <assert.h>
#include <stdio.h>
#include <fcntl.h>

#include <vector>
#include <string>

namespace DOSLIBLinker {

	enum {
		LNKLOG_DEBUGMORE = -2,
		LNKLOG_DEBUG = -1,
		LNKLOG_NORMAL = 0,
		LNKLOG_WARNING = 1,
		LNKLOG_ERROR = 2
	};

	typedef void (*log_callback_t)(const int level,void *user,const char *str);

	void log_callback_silent(const int level,void *user,const char *str);
	void log_callback_default(const int level,void *user,const char *str);

	struct log_t {
		log_callback_t		callback = log_callback_default;
		int			min_level = LNKLOG_NORMAL;
		void*			user = NULL;

		void			log(const int level,const char *fmt,...);
	};

	class file_io {
		public:
			file_io();
			virtual ~file_io();
		public:
			virtual bool			ok(void) const;
			virtual off_t			tell(void);
			virtual bool			seek(const off_t o);
			virtual bool			read(void *buf,size_t len);
			virtual bool			write(const void *buf,size_t len);
			virtual void			close(void);
	};

	class file_stdio_io : public file_io {
		public:
			file_stdio_io();
			file_stdio_io(const char *path,const char *how=NULL);
			virtual ~file_stdio_io();
		public:
			virtual bool			ok(void) const;
			virtual off_t			tell(void);
			virtual bool			seek(const off_t o);
			virtual bool			read(void *buf,size_t len);
			virtual bool			write(const void *buf,size_t len);
			virtual void			close(void);
		public:
			virtual bool			open(const char *path,const char *how=NULL);
		private:
			FILE*				fp = NULL;
	};

	template <typename T> class _base_handlearray {
		public:
			static constexpr size_t			undef = ~((size_t)(0ul));
			typedef T				ent_type;
			typedef size_t				ref_type;
			std::vector<T>				ref;
		public:
			inline size_t size(void) const { return ref.size(); }
		public:
			bool exists(const size_t r) const {
				return r < ref.size();
			}

			const T &get(const size_t r) const {
				return ref.at(r); /* std::vector will throw an exception if out of range */
			}

			T &get(const size_t r) {
				return ref.at(r); /* std::vector will throw an exception if out of range */
			}

			size_t allocate(void) {
				const size_t r = ref.size();
				ref.emplace(ref.end());
				return r;
			}

			size_t allocate(const T &val) {
				const size_t r = ref.size();
				ref.emplace(ref.end(),val);
				return r;
			}
	};

	struct source_t {
		public:
			std::string				path;
			size_t					index = ~((size_t)(0ul));
			off_t					file_offset = off_t(0ul) - off_t(1ul);
	};
	typedef _base_handlearray<source_t> source_list_t;
	typedef source_list_t::ref_type source_ref_t;

	class linkenv {
		public:
			source_list_t				sources;
		public:
			log_t					log;
	};

}

namespace DOSLIBLinker {

	void log_callback_silent(const int level,void *user,const char *str) {
		(void)level;
		(void)user;
		(void)str;
	}

	void log_callback_default(const int level,void *user,const char *str) {
		(void)user;
		fprintf(stderr,"DOSLIBLinker::log level %d: %s\n",level,str);
	}

	void log_t::log(const int level,const char *fmt,...) {
		if (level >= min_level) {
			va_list va;
			const size_t tmpsize = 512;
			char *tmp = new char[tmpsize];

			va_start(va,fmt);
			vsnprintf(tmp,tmpsize,fmt,va);
			callback(level,user,tmp);
			va_end(va);

			delete[] tmp;
		}
	}

	file_io::file_io() {
	}

	file_io::~file_io() {
	}

	bool file_io::ok(void) const {
		return false;
	}

	off_t file_io::tell(void) {
		return 0;
	}

	bool file_io::seek(const off_t o) {
		(void)o;
		return false;
	}

	bool file_io::read(void *buf,size_t len) {
		(void)buf;
		(void)len;
		return false;
	}

	bool file_io::write(const void *buf,size_t len) {
		(void)buf;
		(void)len;
		return false;
	}

	void file_io::close(void) {
	}

	file_stdio_io::file_stdio_io() : file_io() {
	}

	file_stdio_io::file_stdio_io(const char *path,const char *how) : file_io() {
		open(path,how);
	}

	file_stdio_io::~file_stdio_io() {
		close();
	}

	bool file_stdio_io::ok(void) const {
		return (fp != NULL);
	}

	off_t file_stdio_io::tell(void) {
		if (fp != NULL) return ftell(fp);
		return 0;
	}

	bool file_stdio_io::seek(const off_t o) {
		if (fp != NULL) {
			fseek(fp,o,SEEK_SET);
			return (ftell(fp) == o);
		}

		return false;
	}

	bool file_stdio_io::read(void *buf,size_t len) {
		if (fp != NULL && len != size_t(0))
			return (fread(buf,len,1,fp) == 1);

		return false;
	}

	bool file_stdio_io::write(const void *buf,size_t len) {
		if (fp != NULL && len != size_t(0))
			return (fwrite(buf,len,1,fp) == 1);

		return false;
	}

	void file_stdio_io::close(void) {
		if (fp != NULL) {
			fclose(fp);
			fp = NULL;
		}
	}

	bool file_stdio_io::open(const char *path,const char *how) {
		close();

		if (how == NULL)
			how = "rb";

		if ((fp=fopen(path,how)) == NULL)
			return false;

		return true;
	}

}

namespace DOSLIBLinker {

	class OMF_record_data : public std::vector<uint8_t> {
		public:
			OMF_record_data();
			~OMF_record_data();
		public:
			void rewind(void);
			bool seek(const size_t o);
			size_t offset(void) const;
			size_t remains(void) const;
			bool eof(void) const;
		public:
			uint8_t gb(void);
			uint16_t gw(void);
			uint32_t gd(void);
			uint16_t gidx(void);
		private:
			size_t readp = 0;
	};

	struct OMF_record {
		OMF_record_data		data;
		uint8_t			type = 0;
		off_t			file_offset = 0;

		void			clear(void);
	};

	class OMF_record_reader {
		public:
			bool		is_library = false;
			uint32_t	dict_offset = 0;
			uint32_t	block_size = 0;
			uint32_t	dict_size = 0;
			uint8_t		lib_flags = 0;
		public:
			void		clear(void);
			bool		read(OMF_record &rec,file_io &fio,log_t &log);
	};

	class OMF_reader {
		public:
			source_ref_t			current_source = source_list_t::undef;
			OMF_record_reader		recrdr;
		public:
			void				clear(void);
			bool				read(linkenv &lenv,const char *path);
			bool				read(linkenv &lenv,file_io &fio,const char *path);
	};

}

namespace DOSLIBLinker {

	OMF_record_data::OMF_record_data() : std::vector<uint8_t>() {
	}

	OMF_record_data::~OMF_record_data() {
	}

	uint8_t OMF_record_data::gb(void) {
		if ((readp+size_t(1u)) <= size()) { const uint8_t r = *(data()+readp); readp++; return r; }
		return 0;
	}

	uint16_t OMF_record_data::gw(void) {
		if ((readp+size_t(2u)) <= size()) { const uint16_t r = *((uint16_t*)(data()+readp)); readp += 2; return le16toh(r); }
		return 0;
	}

	uint32_t OMF_record_data::gd(void) {
		if ((readp+size_t(4u)) <= size()) { const uint32_t r = *((uint32_t*)(data()+readp)); readp += 4; return le32toh(r); }
		return 0;
	}

	uint16_t OMF_record_data::gidx(void) {
		uint16_t r = gb();
		if (r & 0x80) r = ((r & 0x7Fu) << 8u) + gb();
		return r;
	}

	void OMF_record_data::rewind(void) {
		readp = 0;
	}

	bool OMF_record_data::eof(void) const {
		return readp >= size();
	}

	bool OMF_record_data::seek(const size_t o) {
		if (o <= size()) {
			readp = o;
			return true;
		}
		else {
			readp = size();
			return false;
		}
	}

	size_t OMF_record_data::remains(void) const {
		return size_t(size() - readp);
	}

	size_t OMF_record_data::offset(void) const {
		return readp;
	}

	void OMF_record::clear(void) {
		data.clear();
		data.rewind();
	}

	void OMF_record_reader::clear(void) {
		is_library = false;
		dict_offset = 0;
		block_size = 0;
		dict_size = 0;
		lib_flags = 0;
	}

	bool OMF_record_reader::read(OMF_record &rec,file_io &fio,log_t &log) {
		unsigned char tmp[3],chk;

		rec.clear();
		rec.file_offset = fio.tell();

		if (dict_offset != uint32_t(0)) {
			if (rec.file_offset >= (off_t)dict_offset)
				return false;
		}

		/* <byte: record type> <word: record length> <data> <byte: checksum>
		 *
		 * Data length includes checksum but not type and length fields. */
		if (!fio.read(tmp,3)) return false;
		rec.type = tmp[0];

		const size_t len = le16toh( *((uint16_t*)(tmp+1)) );
		if (len == 0) {
			log.log(LNKLOG_WARNING,"OMF record with zero length");
			return false;
		}

		rec.data.resize(len - size_t(1u)); /* do not include checksum in the data field */
		if (!fio.read(rec.data.data(),len - size_t(1u))) {
			log.log(LNKLOG_WARNING,"OMF record with incomplete data (eof)");
			return false;
		}

		/* now check the checksum */
		if (!fio.read(&chk,1)) {
			log.log(LNKLOG_WARNING,"OMF record with incomplete data (eof on checksum byte)");
			return false;
		}

		/* checksum byte is a checksum byte if the byte value here is nonzero */
		if (chk != 0u) {
			unsigned char sum = 0;

			for (size_t i=0;i < 3;i++) sum += tmp[i];
			for (size_t i=0;i < rec.data.size();i++) sum += rec.data[i];
			sum += chk;

			if (sum != 0u) {
				log.log(LNKLOG_WARNING,"OMF record with bad checksum");
				return false;
			}
		}

		/* In order to read .LIB files properly this code needs to know the block size */
		if (rec.file_offset == 0 && rec.type == 0xF0/*Library Header Record*/) {
			/* <dictionary offset> <dictionary size in blocks> <flags>
			 * The record length is used to indicate block size */
			is_library = true;
			block_size = rec.data.size() + 3/*header*/ + 1/*checksum*/;
			dict_offset = rec.data.gd();
			dict_size = rec.data.gw();
			lib_flags = rec.data.gb();

			/* must be a power of 2 or else it's not valid */
			if ((block_size & (block_size - uint32_t(1ul))) == uint32_t(0ul)) {
				dict_size *= block_size;
			}
			else {
				log.log(LNKLOG_WARNING,"OMF ignoring library header record because block size %lu is not a power of 2",(unsigned long)block_size);
				is_library = false;
			}

			log.log(LNKLOG_DEBUG,"OMF file is library with block_size=%lu, dict_offset=%lu, dict_size=%lu, flags=0x%02x",
				(unsigned long)block_size,(unsigned long)dict_offset,(unsigned long)dict_size,lib_flags);
		}

		if (block_size != uint32_t(0)) {
			if (rec.type == 0x8A/*module end*/ || rec.type == 0x8B/*module end*/) {
				off_t p = fio.tell();
				p += (off_t)(block_size - uint32_t(1u));
				p -= p % (off_t)block_size;
				if (!fio.seek(p)) {
					log.log(LNKLOG_WARNING,"OMF end of module, unable to seek forward to next within library");
					return false;
				}
			}
		}

		return true;
	}

	void OMF_reader::clear(void) {
		current_source = source_list_t::undef;
		recrdr.clear();
	}

	bool OMF_reader::read(linkenv &lenv,const char *path) {
		file_stdio_io fio(path);
		if (fio.ok()) {
			return read(lenv,fio,path);
		}
		else {
			lenv.log.log(LNKLOG_ERROR,"OMF reader: Unable to open and read '%s'",path);
			return false;
		}
	}

	bool OMF_reader::read(linkenv &lenv,file_io &fio,const char *path) {
		OMF_record rec;

		recrdr.clear();

		if (!recrdr.read(rec,fio,lenv.log)) {
			lenv.log.log(LNKLOG_ERROR,"Unable to read first OMF record in '%s'",path);
			return false;
		}

		if (recrdr.is_library) {
			size_t module_index = 0;

			lenv.log.log(LNKLOG_DEBUG,"OMF file '%s' is a library",path);

			while (recrdr.read(rec,fio,lenv.log)) {
				if (rec.type == 0x80/*THEADR*/ || rec.type == 0x82/*LHEADR*/) {
					source_t &src = lenv.sources.get(current_source=lenv.sources.allocate());
					src.file_offset = rec.file_offset;
					src.index = module_index++;
					src.path = path;
				}
			}
		}
		else {
			lenv.log.log(LNKLOG_DEBUG,"OMF file '%s' is an object",path);

			{
				source_t &src = lenv.sources.get(current_source=lenv.sources.allocate());
				src.path = path;
			}
		}

		return true;
	}

}

int main(int argc,char **argv) {
	(void)argc;
	(void)argv;
	DOSLIBLinker::linkenv lenv;
	DOSLIBLinker::OMF_reader omfr;
	lenv.log.min_level = DOSLIBLinker::LNKLOG_DEBUGMORE;
	if (!omfr.read(lenv,"/usr/src/doslib/fmt/omf/testfile/0014.obj")) fprintf(stderr,"Failed to read 0014.obj\n");
	if (!omfr.read(lenv,"/usr/src/doslib/hw/cpu/dos86l/cpu.lib")) fprintf(stderr,"Failed to read cpu.lib\n");

	fprintf(stderr,"Sources:\n");
	for (auto si=lenv.sources.ref.begin();si!=lenv.sources.ref.end();si++) {
		fprintf(stderr,"  [%lu]: path='%s' index=%ld fileofs=%ld\n",
			(unsigned long)(si - lenv.sources.ref.begin()),
			(*si).path.c_str(),
			(signed long)((*si).index),
			(signed long)((*si).file_offset));
	}

	return 0;
}

