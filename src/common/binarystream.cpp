#include "binarystream.h"
#include <iostream>
#include <ctime>
#include <astro/util.h>
#include <iostream>

using namespace std;

// random magic number
const int header::magic = 0x37592664;
const int header::current_version = 1;

void ibinarystream::read(char *v, size_t n)
{
	f.read(v, n);
}

void obinarystream::write(const char *v, size_t n)
{
	f.write(v, n);
}

header::header(std::string description_)
: description(description_), datetime(time(NULL)), version(current_version)
{
}

header::header()
: description("Unititialized header"), datetime(0), version(-1)
{
}

obinarystream &operator <<(obinarystream &out, const header::data_map &data)
{
	out << data.size();
	FOREACH(header::data_map::const_iterator, data) { out << (*i).first << (*i).second; }
	return out;
}

ibinarystream &operator >>(ibinarystream &in, header::data_map &data)
{
	size_t size; string k, v;

	data.clear();

	in >> size;
	FOR(0, size) { in >> k >> v; data[k] = v; }
	return in;
}

obinarystream &operator <<(obinarystream &out, const header &h)
{
	out << header::magic << h.version << h.description << h.datetime << h.data;
	return out;
}

ibinarystream &operator >>(ibinarystream &in, header &h) throw (EBinaryIO)
{
	int magic = 0;
	in >> magic;
	if(magic != header::magic) { THROW(EBinaryIO, "This file does not start with a standard binary header. Perhaps the file has no header information, is compressed or corrupted?"); }

	in >> h.version >> h.description >> h.datetime >> h.data;

	return in;
}

OSTREAM(const header &h)
{
	cout << h.description << "\n\n";

	cout << "Header keywords:" << "\n";
	FOREACH(header::data_map::const_iterator, h.data) { cout << "    " << (*i).first << " = " << (*i).second << "\n"; }
	cout << "\n";
	
	cout << "File saved on " << ctime(&h.datetime) << "\n";
	cout << "Internal header version: " << h.version << "\n";
	cout << "This code can read headers up to version: " << header::current_version << "\n";
}