/***************************************************************************
 *   Copyright (C) 2004 by Mario Juric                                     *
 *   mjuric@astro.Princeton.EDU                                            *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#include "config.h"
 
#include "model.h"
#include "textstream.h"
#include "analysis.h"

#include <iostream>
#include <fstream>

#include <astro/system/options.h>
#include <astro/exceptions.h>
#include <astro/util.h>
#include <astro/math.h>
#include <astro/system/log.h>
#include <astro/io/format.h>
#include <astro/useall.h>

#include <gsl/gsl_poly.h>

#if HAVE_BOOST_REGEX
#include <boost/regex.hpp>
#endif

using namespace std;


// Get the autoconf/automake set datadir
const std::string &datadir()
{
	static std::string dd;
	static const char *dd_hardcoded = DATADIR;
	if(dd.empty())
	{
		EnvVar ev("DATADIR");
		dd = ev ? ev.c_str() : dd_hardcoded;
		DLOG(verb2) << "datadir=" << dd << (ev ? " (initializes from $DATADIR)" : "");
	}
	return dd;
}

///////////////////////////////////////////////
otable::kv *otable::parse(const std::string &defs, otable::parse_callback *cback)
{
	// parse stringDef and create a corresponding tag instance
	// the tag definition format is: ((class|column)) name [n] { field1=value1; field2=value2; ...}
	std::istringstream ss(defs);
	char c;
	kv *kvobj = NULL;
	std::string what, name;
	int ncolumn = 0;
	while(ss >> c)
	{
		// find out the metatype of object being parsed (class or column).
		ss.unget();
		if(c == '(') { ss >> what; }
		else { what = "(column)"; }

		// load the name of the object being parsed
		name.clear();
		ss >> c;	// eats any whitespace to next character
		while(ss && (isalnum(c) || c == '_' || c == ':')) { name += c; ss.get(c); }
		ss.unget();
		if(name.empty())
		{
			THROW(EAny, "Error reading column name at character " + str((size_t)ss.tellg()) + " of the line.");
		}

		// get or instantiate this object
		if(what == "(class)")
		{
			if(!cclasses.count(name))
			{
				cclasses[name].reset(new columnclass(*this));
			}
			kvobj = cclasses[name].get();
		}
		else if(what == "(column)")
		{
			if(!columns.count(name))
			{
				columns[name].reset(new columndef(*this));
			}
			kvobj = columns[name].get();

/*			if(setInputOn)
			{
				columns[name]->input = ncolumn;
				ncolumn++;
			}*/
		}
		else
		{
			THROW(EAny, "Expected 'class' or 'column', got " + what);
		}
		kvobj->set_property("__name__", name);

		ss >> c;	// look for '[' or '{'
		// special (traditional) syntax for arrays
		if(c == '[' && what == "(column)")
		{
			size_t width;
			ss >> width;
			kvobj->set_property("n", str(width));
//			std::cerr << what << " " << name << ": [] =" << width << "\n";

			ss >> c;	// look for ']'
			if(c != ']') { THROW(EAny, "Expected ']', got " + str(c)); }

			ss >> c;	// look for '{'
		}

		if(c != '{')
		{
			if(what == "(column)") { ss.unget(); if(cback) (*cback)(kvobj); continue; }	// bare column defininiton (without details)
			THROW(EAny, "Expected '{', got " + str(c));
		}

		ss >> c;
		while(c != '}')
		{
			ss.unget();

			std::string key;
			ss >> c; while(ss && (isalnum(c) || c == '_')) { key += c; ss.get(c); }
			if(!ss) { THROW(EAny, "End of file while reading key name"); }
			if(c != '=') {
				THROW(EAny, "Expected '=', got " + str(c));
			}

			std::string value;
			ss >> c; while(ss && (c != ';' && c != '}')) { value += c; ss.get(c); }
			if(!ss) { THROW(EAny, "End of file while reading field name"); }
			if(c == '}') { ss.unget(); }

//			std::cerr << what << " " << name << ": " << key << "=" << value << "\n";
			kvobj->set_property(key, value);

			ss >> c;
		}

		if(cback) (*cback)(kvobj);
	}
	return kvobj;
}

//void serialize(fmtout &line, const size_t row) const;
//void unserialize(std::istream &in, const size_t row);

template<typename T>
struct default_column_type_traits : public column_type_traits
{
	virtual void  serialize(fmtout &out, const std::string &format, const void *val) const { const T *v = reinterpret_cast<const T*>(val); out.printf(format, *v); }
	virtual void  unserialize(void *val, std::istream &in) const { T *v = reinterpret_cast<T*>(val); in >> *v; }
	virtual void* constructor(void *p) const { return new (p) T(); }
	virtual void  destructor(void *val) const { reinterpret_cast<T*>(val)->~T(); }

	default_column_type_traits(const std::string &name) : column_type_traits(name, sizeof(T)) {}
};

// These are C type->traits mappings. Specialize them for each datatype supported by column_type_traits::get
// and declare them in model.h (or else it won't work!!)
template<> const column_type_traits *column_type_traits::get<float>()  { return column_type_traits::get("float"); }
template<> const column_type_traits *column_type_traits::get<int>()    { return column_type_traits::get("int"); }
template<> const column_type_traits *column_type_traits::get<double>() { return column_type_traits::get("double"); }
template<> const column_type_traits *column_type_traits::get<char>()   { return column_type_traits::get("char"); }

std::map<std::string, boost::shared_ptr<column_type_traits> > column_type_traits::defined_types;
const column_type_traits *column_type_traits::get(const std::string &datatype)
{
	static bool initialized = false;
	if(!initialized)
	{
		#define ADDTYPE(strT, T) defined_types[strT].reset(new default_column_type_traits<T>(strT));
		ADDTYPE("int", int);
		ADDTYPE("double", double);
		ADDTYPE("char", char);
		ADDTYPE("float", float);
		#undef CREATETYPE
		initialized = true;
	}

	if(!defined_types.count(datatype)) { THROW(EAny, "Unknown tag data type '" + datatype + "'"); }
	return defined_types[datatype].get();
}

otable::columnclass::columnclass(otable &parent_)
	: parent(parent_), kv("(class)")
{
	typeProxy = column_type_traits::get("float");	// default column type
	ASSERT(typeProxy);
}

otable::columndef::~columndef()
{
	dealloc();
}

otable::columndef::columndef(otable &parent_)
	: parent(parent_), kv("(column)")
{
	// defaults
	columnClass = parent.cclasses["default"].get();
	typeProxy = NULL;	// default to class type
	m_hidden = false;		// default to outputing the column
}

void otable::columndef::alloc(const size_t nrows)
{
	// do nothing if the length is OK
	if(nrows == ptr.nrows()) { return; }

	dealloc();

	const column_type_traits *tt = type();
	size_t elementSize = tt->elementSize;
	ptr.resize(nrows, ptr.width(), elementSize);

	// call constructors
	char *base = ptr.get();
	size_t pitch = ptr.pitch();
	for(size_t i = 0; i != ptr.width(); i++)
	{
		for(size_t j = 0; j != ptr.nrows(); j++)
		{
			void *Aij = base + pitch*i + elementSize*j;
			tt->constructor(Aij);
		}
	}
}

void otable::columndef::dealloc()
{
	if(!ptr.size()) { return; }

	// call destructors
	const column_type_traits *tt = type();
	size_t elementSize = ptr.elementSize();
	size_t pitch = ptr.pitch();
	char *base = ptr.get();
	for(size_t i = 0; i != ptr.width(); i++)
	{
		for(size_t j = 0; j != ptr.nrows(); j++)
		{
			void *Aij = base + pitch*i + elementSize*j;
			tt->destructor(Aij);
		}
	}

	// deallocate the memory without erasing the metadata
	ptr.resize(0, ptr.width());
}

void otable::columnclass::set_property(const std::string &key, const std::string &value)
{
	if(key == "__name__")
	{
		if(className.empty()) { className = value; }
		return;
	}

	if(key == "fmt") { formatString = value; return; }

	if(key == "type")
	{
		typeProxy = column_type_traits::get(value);
		ASSERT(typeProxy);
		return;
	}

	// default: store the property in m_properties map
	m_properties[key] = value;
}

void otable::columndef::set_property(const std::string &key, const std::string &value)
{
	if(key == "fmt") { formatString = value; return; }

	if(key == "__name__")
	{
		ASSERT(columnName.empty() || columnName == value);
		columnName = value;

		return;
	}

	if(key == "alias")
	{
		if(!parent.columns.count(value))
		{
			ASSERT(!columnName.empty());
			parent.columns[value] = parent.columns[columnName];
		}
		ASSERT(parent.columns[value] == parent.columns[columnName]);
		return;
	}

	if(key == "class")
	{
		ASSERT(parent.cclasses.count(value));
		dealloc();
		columnClass = parent.cclasses[value].get();
		return;
	}

	if(key == "n") // vector width
	{
		dealloc();
		int width = atoi(value.c_str());
		ASSERT(width > 1);
		ptr.resize(ptr.nrows(), width);
		return;
	}

	if(key == "type")
	{
		dealloc();
		typeProxy = column_type_traits::get(value);
		return;
	}

	if(key == "hidden")
	{
		m_hidden = value == "true" || atoi(value.c_str()) != 0;
		return;
	}

	if(key == "fieldNames")
	{
		// value = "idx:fieldname,idx:fieldname,..."
		size_t at = 0;
		size_t len;
		do
		{
			len = value.find(",", at);
			if(len != std::string::npos) { len -= at; }
			std::string pair = value.substr(at, len);
			at += len+1;

			int semi = pair.find(':');
			ASSERT(semi != std::string::npos);

			std::string sidx = pair.substr(0, semi);
			std::string name = pair.substr(semi+1);
			int idx = atoi(sidx.c_str());

			fieldNames.str2idx[name] = idx;
			fieldNames.idx2str[idx] = name;
		} while(len != std::string::npos);
		return;
	}

	// default: store the property in m_properties map
	m_properties[key] = value;
}

void otable::columndef::serialize(fmtout &line, const size_t row) const
{
	const column_type_traits *tt = type();
// 	char *at = (char*)data + tt->elementSize*row;
	const char *at = ((column<char> &)ptr).get() + tt->elementSize*row;
	const std::string &fmt = getFormatString();

	FOR(0, ptr.width())
	{
		tt->serialize(line, fmt, at);
		at += ptr.pitch();
	}
}
void otable::columndef::unserialize(std::istream &in, const size_t row)
{
	const column_type_traits *tt = type();
//	char *at = (char*)data + tt->elementSize*row;
	char *at = ptr.get() + tt->elementSize*row;
	FOR(0, ptr.width())
	{
		tt->unserialize(at, in);
		at += ptr.pitch();
	}
}

size_t otable::columndef::setFieldNames(const std::map<int, std::string> &names)
{
	fieldNames.str2idx.clear();
	fieldNames.idx2str.clear();

	FOREACH(names)
	{
		assert(!fieldNames.idx2str.count(i->first));
		assert(!fieldNames.str2idx.count(i->second));

		fieldNames.idx2str[i->first] = i->second;
		fieldNames.str2idx[i->second] = i->first;
	}
}

size_t otable::columndef::getFieldNames(std::map<int, std::string> &names) const
{
	names = fieldNames.idx2str;
}

size_t otable::columndef::getFieldNames(std::set<std::string> &names) const
{
	FOREACH(fieldNames.str2idx)
	{
		names.insert(i->first);
	}
}

void otable::columndef::serialize_def(std::ostream &out) const
{
	out << columnName;
	if(ptr.width() > 1) { out << "[" << ptr.width() << "]"; }

	const otable::columndef *dflt = NULL;
	if(parent.columns.count("default::" + columnName))
	{
		dflt = parent.columns.at("default::" + columnName).get();
	}

	std::stringstream ss;
	// keywords that have changed from their defaults
	#define DFLT(var) (dflt && dflt->var == var)
	if(typeProxy                           && !DFLT(typeProxy))    { ss << "type=" << typeProxy->typeName << ";"; }
	if(columnClass->className != "default" && !DFLT(columnClass))  { ss << "class=" << columnClass->className << ";"; }
	if(!formatString.empty()               && !DFLT(formatString)) { ss << "fmt=" << formatString << ";"; }
	if(dflt                                && !DFLT(m_hidden))     { ss << "hidden=" << m_hidden << ";"; }
	#undef DFLT

	// fieldNames
	if(fieldNames.idx2str.size())
	{
		ss << "fieldNames=";
		bool first = true;
		FOREACH(fieldNames.idx2str)
		{
			if(!first) { ss << ","; }
			ss << i->first << ":" << i->second;
			first = false;
		}
		ss << ";";
	}

	// aliases
	FOREACH(parent.columns)
	{
		if(i->second.get() != this) { continue; }
		if(i->first == columnName) { continue; }
		ss << "alias=" << i->first << ";";
	}

	// properties
	FOREACH(m_properties)
	{
		const std::string &key = i->first, &val = i->second;

		// ignore if this property is there by default
		if(dflt && dflt->get_property(key) == val) { continue; }

		ss << i->first << "=" << i->second << ";";
	}

	// output details only if they differ from defaults
	std::string details = ss.str();
	if(!details.empty())
	{
		out << "{" << details << "}";
	}
}

void otable::getColumnsForOutput(std::vector<const columndef*> &outColumns) const
{
	FOREACH(colOutput)
	{
		if(!columns.count(*i)) continue;		// don't display columns that have not been created
		if(columns.at(*i)->hidden()) continue;		// don't display hidden columns

		outColumns.push_back(columns.at(*i).get());
	}
}

std::ostream& otable::serialize_header(std::ostream &out) const
{
	std::vector<const columndef*> outColumns;
	getColumnsForOutput(outColumns);

	FOREACH(outColumns)
	{
		(*i)->serialize_def(out);
		out << " ";
	}
	return out;
}

// serialization/unserialization routines
size_t otable::serialize_body(std::ostream& out, size_t from, size_t to, const mask_functor &mask) const
{
	ASSERT(from >= 0);
	if(to > size()) { to = size(); }

	std::vector<const columndef*> outColumns;
	getColumnsForOutput(outColumns);

	size_t cnt = 0;
	FORj(row, from, to)
	{
		if(!mask.shouldOutput(row)) { continue; }
		cnt++;

		fmtout line;
		FOREACH(outColumns)
		{
			(*i)->serialize(line, row);
		}
		out << line.c_str() << "\n";
	}
	return cnt;
};

void otable::getColumnsForInput(std::vector<columndef*> &inColumns)
{
	FOREACH(colInput)
	{
		ASSERT(columns.count(*i));
		inColumns.push_back(&getColumn(*i));
	}
}

struct record_loaded_columns : public otable::parse_callback
{
	std::vector<otable::columndef *> columns;

	virtual bool operator()(otable::kv *kvobj)
	{
//		std::cout << "META: " << kvobj->what << "\n";
		if(kvobj->what != "(column)") return true;
		columns.push_back(static_cast<otable::columndef*>(kvobj));

		return true;
	}
};

std::istream& otable::unserialize_header(std::istream &in, std::set<std::string> *columns)
{
	// gobble-up an optional the comment sign
	char c;
	in >> c;
	if(c != '#') { in.unget(); }

	// gobble up the rest of the line, where we expect the definitions to sit
	std::string line;
	getline(in, line);

	record_loaded_columns lc;
	parse(line, &lc);
	FOREACH(lc.columns)
	{
		colInput.push_back((*i)->columnName);
		if(columns) { columns->insert((*i)->columnName); }
	}

	colOutput = colInput;	// Output all unserialized columns by default

	return in;
}

std::istream& otable::unserialize_body(std::istream& in)
{
	std::vector<columndef*> inColumns;
	getColumnsForInput(inColumns);

	nrows = 0;
	FORj(row, 0, length)	// read at most length rows
	{
		bool first = true;
		FOREACH(inColumns)
		{
			(*i)->unserialize(in, row);
			if(!in) { break; }
			first = false;
		}
		if(!in) {
			if(!first) { THROW(EAny, "Incomplete last line."); }
			break;
		}
		nrows++;
	}

	if(!in && !in.eof()) { THROW(EAny, "Error after reading " + str(nrows) + " rows."); }

	return in;
};

size_t otable::set_output(const std::string &colname, bool output)
{
	std::vector<std::string>::iterator it = find(colOutput.begin(), colOutput.end(), colname);
	if(it != colOutput.end())
	{
		if(output) { return it - colOutput.begin(); }
		colOutput.erase(it);
		return -1;
	}

	if(output)
	{
		colOutput.push_back(colname);
		return colOutput.size()-1;
	}

	return -1;
}

size_t otable::set_output_all(bool output)
{
	// output all columns that are in use
	colOutput.clear();
	FOREACH(columns)
	{
		if(i->first != i->second->columnName) { continue; }
		if(i->second->capacity() == 0) { continue; }
		set_output(i->first, output);
	}
}

otable::columndef &otable::use_column(const std::string &coldef, bool setOutput)
{
	columndef *col = (columndef *)parse("(column) " + coldef);
	ASSERT(col != NULL);

	col->alloc(length); // Ensure the column is allocated

	if(setOutput) { set_output(col->columnName, true); }

	return *col;
}

otable::columndef &otable::use_column_by_cloning(const std::string &newColumnName, const std::string &existingColumnName, std::map<int, std::string> *newFieldNames, bool setOutput)
{
	ASSERT(columns.count(newColumnName) == 0);

	columndef &exCol = getColumn(existingColumnName);
	boost::shared_ptr<columndef> col = columns[newColumnName] = exCol.clone(newColumnName, newFieldNames);

	col->alloc(length); // Ensure the column is allocated

	if(setOutput) { set_output(col->columnName, true); }

	return *col;
}

void otable::init()
{
	// definition of built-in classes and column defaults
	parse(
	"(class) default      {fmt=% 7.3f;}" 			// NOTE: This class must come be defined before any columns are ever instantiated
	"(class) magnitude    {fmt=% 7.3f;}" 			// -12.345
	"(class) color        {fmt=% 6.3f;}"			// -12.345
	"(class) astrometry   {fmt=% 13.8f; type=double;}"	// -123.12345678
	"(class) position     {fmt=% 10.2f;}"			// -123456.78
	"(class) propermotion {fmt=% 7.1f;}"			// -1234.1
	"(class) velocity     {fmt=% 7.1f;}"			// -1234.1
	"(class) flags        {fmt=% 4d; type=int;}"		// 1234

	// definition of built-in fields
	"(column) comp          {type=int; fmt=%3d;}"
	"(column) radec[2]      {class=astrometry;}"
	"(column) lb[2]         {class=astrometry;}"
	"(column) XYZ [3]       {class=position;}"
	"(column) FeH           {fmt=% 6.3f;}"
	"(column) vcyl[3]       {class=velocity;}"
	"(column) pmlb[3]       {class=propermotion;}"
	"(column) pmradec[3]    {class=propermotion;}"
	"(column) star_name[40] {type=char;}"
	"(column) hidden	{type=int;hidden=true;}"
	"(column) projIdx       {type=int;hidden=true;}"
	);

	// store these column definitions as defaults
	typeof(columns) tmp(columns);
	FOREACH(tmp)
	{
		if(i->second->columnName != i->first) { continue; } // skip aliases

		std::string newName = "default::" + i->first;
		columns[newName] = i->second->clone(newName);
	}
}

size_t otable::get_used_columns(std::set<std::string> &cols) const
{
	cols.clear();
	FOREACH(columns)
	{
		if(i->second->capacity() == 0) { continue; }
		cols.insert(i->first);
	}
	return cols.size();
}

size_t otable::get_used_columns_by_class(std::set<std::string> &cols, const std::string &className) const
{
	cols.clear();
	FOREACH(columns)
	{
		if(i->second->capacity() == 0) { continue; }
		if(i->second->className() != className) { continue; }
		cols.insert(i->first);
	}
	return cols.size();
}

otable::columndef &otable::getColumn(const std::string &name)
{
	// Auto-create if needed
	if(!columns.count(name))
	{
		// autocreate
		use_column(name);
	}
	ASSERT(columns.count(name))
	{
		std::cerr << "Column " << name << " doesn't exist and couldn't be autocreated?!";
	}
 	columndef &col = *columns[name].get();

	// Auto-create column data
	if(col.capacity() != length)
	{
		col.alloc(length);
		ASSERT(col.capacity() == length);
	}

	return col;
}

/////////////

const char *disk_model::param_name[disk_model::nparams] =
{
	"rho0", "l", "h", "z0", "f", "lt", "ht", "fh", "q", "n",
	"rho1", "rho2", "rho3", "rho4", "rho5", "rho6", "rho7", "rho8", "rho9", "rho10", 
};
const char *disk_model::param_format[disk_model::nparams] = 
{
	"%.5f", "%.0f", "%.0f", "%.2f", "%.3f", "%.0f", "%.0f", "%.5f", "%.2f", "%.2f",
	"%.5f", "%.5f", "%.5f", "%.5f", "%.5f", "%.5f", "%.5f", "%.5f", "%.5f", "%.5f"
};

/////////////

void model_fitter::get_parameters(gsl_vector *x)
{
	int k = 0;
	FOR(0, nparams)
	{
		if(fixed[i]) continue;
		gsl_vector_set(x, k++, p[i]);
	}
}

void model_fitter::set_parameters(const gsl_vector *x)
{
	int k = 0;
	FOR(0, nparams)
	{
		if(fixed[i]) continue;
		p[i] = gsl_vector_get(x, k++);
	}
}

#define DFINIT int pcnt_ = 0, j_ = 0;
#define DFCALC(val) if(!fixed[pcnt_++]) gsl_matrix_set(J, i, j_++, (val)/x.sigma);
int model_fitter::fdf (gsl_vector * f, gsl_matrix * J)
{
	// calculate f_i values for all datapoints
	FOR(0, map.size())
	{
		const rzpixel &x = map[i];

		int ri = x.ri_bin;

		double rhothin = rho_thin(x.r, x.z, ri);
		double rhothick = rho_thick(x.r, x.z, ri);
		double rhohalo = rho_halo(x.r, x.z, ri);
		double rhom = rhothick + rhothin + rhohalo;

		if(f)
		{
			double df = rhom - x.rho;
			gsl_vector_set(f, i, df/x.sigma);
		}

		if(J)
		{
			DFINIT;
			DFCALC(drho0(x.r, x.z, rhom, ri, 0));
			DFCALC(dl(x.r, x.z, rhothin));
			DFCALC(dh(x.r, x.z, rhothin));
			DFCALC(dz0(x.r, x.z, rhothin, rhothick));
			DFCALC(df(x.r, x.z, rhothick));
			DFCALC(dlt(x.r, x.z, rhothick));
			DFCALC(dht(x.r, x.z, rhothick));
			DFCALC(dfh(x.r, x.z, rhohalo));
			DFCALC(dq(x.r, x.z, rhohalo));
			DFCALC(dn(x.r, x.z, rhohalo));
			DFCALC(drho0(x.r, x.z, rhom, ri, 1));
			DFCALC(drho0(x.r, x.z, rhom, ri, 2));
			DFCALC(drho0(x.r, x.z, rhom, ri, 3));
			DFCALC(drho0(x.r, x.z, rhom, ri, 4));
			DFCALC(drho0(x.r, x.z, rhom, ri, 5));
			DFCALC(drho0(x.r, x.z, rhom, ri, 6));
			DFCALC(drho0(x.r, x.z, rhom, ri, 7));
			DFCALC(drho0(x.r, x.z, rhom, ri, 8));
			DFCALC(drho0(x.r, x.z, rhom, ri, 9));
			DFCALC(drho0(x.r, x.z, rhom, ri, 10));
		}

	}

	return GSL_SUCCESS;
}
#undef DFCALC
#undef DFINIT

void model_fitter::cull(double nSigma)
{
	// calculate f_i values for all datapoints
	map.clear();
	culled.clear();
	FOR(0, orig.size())
	{
		const rzpixel &x = orig[i];
		double rhom = rho(x.r, x.z, x.ri_bin);
		if(abs(x.rho - rhom) <= nSigma*x.sigma)
		{
			map.push_back(x);
		} else {
			culled.push_back(x);
		}
	}
	cerr << "Selected " << map.size() << " out of " << orig.size() << " pixels\n";
}

void model_fitter::residual_distribution(std::map<int, int> &hist, double binwidth)
{
	// calculate f_i values for all datapoints
	FOR(0, map.size())
	{
		const rzpixel &x = map[i];
		double rhom = rho(x.r, x.z, x.ri_bin);
		double r = (x.rho - rhom) / x.sigma;
		int ir = (int)floor((r+0.5*binwidth) / binwidth);

		if(hist.find(ir) == hist.end()) hist[ir] = 1;
		else hist[ir]++;
	}
}

void model_fitter::print(ostream &out, int format, int ri_bin)
{
	int riidx = ri2idx(ri_bin);
	switch(format)
	{
	case PRETTY:
		out << io::format("%15s = (%.3f, %.3f)") << "ri" << ri[ri_bin].first << ri[ri_bin].second << "\n";
		out << io::format("%15s = %d") << "n(DOF)" << ndof() << "\n";
		out << io::format("%15s = %.5g") << "chi^2/dof" << chi2_per_dof << "\n";
		out << io::format("%15s = %.5g") << "eps{abs,rel}" << epsabs << " " << epsrel << "\n";
		FOR(0, nparams)
		{
			out << io::format(std::string("%15s = ") + param_format[i]) << param_name[i] << p[i];
			out << " +- " << io::format(param_format[i]) << sqrt(variance(i));
			out << (fixed[i] ? " (const)" : " (var)");
			out << "\n";
		}
		out << "\n";
		if(covar.size())
		{
			FORj(r, -1, nparams) // rows
			{
				if(r == -1) { out << io::format("%15s = ") << "corr. matrix"; }
				else { out << io::format("%15s = ") << param_name[r]; }
	
				FORj(c, 0, nparams) // columns
				{
					if(r == -1) { out << io::format(" %10s") << param_name[c]; continue; }
					double corr = fixed[c] || fixed[r] ? 0 : covar[r*nparams + c] / sqrt(variance(c)*variance(r));
					std::cerr << io::format(" %10.3g") << corr;
				}
				out << "\n";
			}
		}
		break;
	case HEADING:
		out << "# ";
		FOR(0, nparams)
		{
			out << param_name[i] << " ";
		}
		out << "\n# ";
		FOR(0, fixed.size())
		{
			out << (fixed[i] ? "const" : "var") << " ";
		}
		break;
	case LINE:
		// parameters
		FORj(k, 0, nparams - nrho)
		{
			int i = k == 0 ? riidx : k;
			out << io::format(param_format[i]) << p[i];
			if(i != nparams-1) { out << " "; }
		}
		out << "       ";
		// errors
		FORj(k, 0, nparams - nrho)
		{
			int i = k == 0 ? riidx : k;
			out << io::format(param_format[i]) << sqrt(variance(i));
			if(i != nparams-1) { out << " "; }
		}
	}
}

/// spline class

spline::spline(const double *x, const double *y, int n)
	: f(NULL), acc(NULL)
{
	construct(x, y, n);
}

spline& spline::operator= (const spline& a)
{
	if(a.f != NULL && a.acc != NULL)
	{
		construct(&a.xv[0], &a.yv[0], a.xv.size());
	} else {
		f = NULL; acc = NULL;
	}
	return *this;
}

void spline::construct(const double *x, const double *y, int n)
{
	// copy data
	xv.resize(n); yv.resize(n);
	copy(x, x+n, &xv[0]);
	copy(y, y+n, &yv[0]);
	
	construct_aux();
}

void spline::construct_aux()
{
	// construct spline
	f = gsl_interp_alloc(gsl_interp_linear, xv.size());
	gsl_interp_init(f, &xv[0], &yv[0], xv.size());
	acc = gsl_interp_accel_alloc();
}

spline::~spline()
{
	if(acc != NULL) gsl_interp_accel_free(acc);
	if(f != NULL) gsl_interp_free(f);
}

BOSTREAM2(const spline &spl)
{
	return out << spl.xv << spl.yv;
}

BISTREAM2(spline &spl)
{
	if(!(in >> spl.xv >> spl.yv)) return in;
	ASSERT(spl.xv.size() == spl.yv.size());
	if(spl.xv.size()) { spl.construct_aux(); }
	return in;
}

////////////////////////////////////////////////////

double ToyHomogeneous_model::rho(double x, double y, double z, double ri)
{
	return rho0;
}

ToyHomogeneous_model::ToyHomogeneous_model(peyton::system::Config &cfg)
	: galactic_model(cfg)
{
	cfg.get(rho0, "rho0", rho0);
	DLOG(verb1) << "rho0 = " << rho0;
}

peyton::io::obstream& ToyHomogeneous_model::serialize(peyton::io::obstream& out) const
{
	galactic_model::serialize(out);
	out << rho0;

	return out;
}

ToyHomogeneous_model::ToyHomogeneous_model(peyton::io::ibstream &in)
	: galactic_model(in)
{
	in >> rho0;
	ASSERT(in);
}

////////////////////////////////////////////////////

double ToyGeocentricPowerLaw_model::rho(double x, double y, double z, double ri)
{
	x -= Rg();
	double d2 = sqr(x) + sqr(y) + sqr(z);
	double norm = lf.empty() ? 1. : lf(ri);
	return norm * rho0 * pow(d2, n/2.);
}

ToyGeocentricPowerLaw_model::ToyGeocentricPowerLaw_model(peyton::system::Config &cfg)
	: galactic_model(cfg)
{
	cfg.get(rho0,  "rho0",  rho0);
	cfg.get(n, "n", n);

	if(cfg.count("lumfunc"))
	{
		text_input_or_die(in, cfg["lumfunc"]);
		vector<double> ri, phi;
		::load(in, ri, 0, phi, 1);
		lf.construct(ri, phi);
	}

	DLOG(verb1) << "rho0 = " << rho0 << ", n = " << n;
}

peyton::io::obstream& ToyGeocentricPowerLaw_model::serialize(peyton::io::obstream& out) const
{
	galactic_model::serialize(out);
	out << rho0 << n << lf;

	return out;
}
ToyGeocentricPowerLaw_model::ToyGeocentricPowerLaw_model(peyton::io::ibstream &in)
	: galactic_model(in)
{
	in >> rho0 >> n >> lf;
	ASSERT(in);
}


void BahcallSoneira_model::load(peyton::system::Config &cfg)
{
	FOREACH(cfg) { MLOG(verb1) << (*i).first << " = " << (*i).second; }

	FOR(0, m.nparams - m.nrho)
	{
		std::string param = m.param_name[i];
		ASSERT(cfg.count(param)) { std::cerr << "Initial value for " << param << " not specified\n"; }

		m.p[i] = cfg[param];
	}
	
	// luminosity function
	if(cfg.count("lumfunc"))
	{
		if(cfg.count("rho0_ri") == 0)
		{
			rho0_ri = make_pair(0., 0.);
		} else {
			rho0_ri = cfg["rho0_ri"];
		}

		input_or_die(in, cfg["lumfunc"]);
		load_luminosity_function(in, rho0_ri);
	}

	// cutoff radius (default: 100kpc)
	cfg.get(r_cut2,  "rcut",   1e5);
	r_cut2 *= r_cut2;
}

BahcallSoneira_model::BahcallSoneira_model(peyton::system::Config &cfg)
	: galactic_model(cfg)
{
	load(cfg);
}

bool BahcallSoneira_model::add_details(otable &t, rng_t &rng)
{
	using namespace column_types;
	cfloat::host_t XYZ = t.col<float>("XYZ");
	cint::host_t  comp = t.col<int>("comp");

	for(size_t row = 0; row != t.size(); row++)
	{
		float x = XYZ(row, 0);
		float y = XYZ(row, 1);
		float z = XYZ(row, 2);
		float r = sqrt(x*x + y*y);

		float thin = m.rho_thin(r, z, 0);
		float thick = m.rho_thick(r, z, 0);
		float halo = m.rho_halo(r, z, 0);
		float rho = thin+thick+halo;

		float pthin  = thin / rho;
		float pthick = (thin + thick) / rho;

		float u = rng.uniform();
		if(u < pthin) { comp[row] = THIN; }
		else if(u < pthick) { comp[row] = THICK; }
		else { comp[row] = HALO; }
	}

	return true;
}

double BahcallSoneira_model::rho(double x, double y, double z, double ri)
{
	double r = sqrt(x*x + y*y);
	double norm = lf.empty() ? 1. : lf(ri);
//	norm = 1.;
	double rho = norm * m.rho(r, z, 0);

#if 1
	// Galactocentric cutoff: model it as a smooth transition, so that the integrator driver doesn't barf
	// The exponential is an analytic approximation of a step function
	double rc = (x*x + y*y + z*z) / r_cut2 - 1;

	double f;
	     if(rc < -0.01) { f = 1.; }
	else if(rc > 0.01)  { f = 0.; }
	else                { f = 1. / (1. + exp(1000 * rc)); };

	rho = rho * f;
#else
	// simple cutoff
	if(x*x + y*y + z*z > r_cut2) { return 0; }
#endif
	return rho;
}

void BahcallSoneira_model::load_luminosity_function(istream &in, std::pair<double, double> rho0_ri)
{
	// load the luminosity function and normalize to m.rho0.
	// rho0 is assumed to contain the number of stars per cubic parsec
	// per 1mag of r-i
	itextstream lfin(in);
	vector<double> ri, phi;
	::load(lfin, ri, 0, phi, 1);
	lf.construct(ri, phi);

	// make the LF dimensionless
	double dr = rho0_ri.second - rho0_ri.first;
	if(dr > 0) {
		double stars_per_mag = lf.integral(rho0_ri.first, rho0_ri.second) / dr;
		FOREACH(phi) { *i /= stars_per_mag; };
	}
	lf.construct(ri, phi);
}

BLESS_POD(disk_model);
peyton::io::obstream& BahcallSoneira_model::serialize(peyton::io::obstream& out) const
{
	galactic_model::serialize(out);
	out << m << lf << r_cut2;

	return out;
}
BahcallSoneira_model::BahcallSoneira_model(peyton::io::ibstream &in)
	: galactic_model(in)
{
	in >> m >> lf >> r_cut2;
	ASSERT(in);
}


galactic_model *galactic_model::load(istream &cfgstrm)
{
	Config cfg;
	cfg.load(cfgstrm);

	FOREACH(cfg) { DLOG(verb1) << (*i).first << " = " << (*i).second; }
	if(cfg.count("model") == 0) { ASSERT(0); return NULL; }

	std::string model = cfg["model"];

	if(model == "BahcallSoneira") { return new BahcallSoneira_model(cfg); }
	if(model == "ToyHomogeneous") { return new ToyHomogeneous_model(cfg); }
	if(model == "ToyGeocentricPowerLaw") { return new ToyGeocentricPowerLaw_model(cfg); }

	ASSERT(0); return NULL;
}

galactic_model *galactic_model::unserialize(peyton::io::ibstream &in)
{
	std::string model;
	in >> model;

	if(model == "BahcallSoneira")
	{
		std::auto_ptr<BahcallSoneira_model> m(new BahcallSoneira_model(in));
		return m.release();
	}
	if(model == "ToyHomogeneous")
	{
		std::auto_ptr<ToyHomogeneous_model> m(new ToyHomogeneous_model(in));
		return m.release();
	}
	if(model == "ToyGeocentricPowerLaw")
	{
		std::auto_ptr<ToyGeocentricPowerLaw_model> m(new ToyGeocentricPowerLaw_model(in));
		return m.release();
	}

	ASSERT(0); return NULL;
}

peyton::io::obstream& galactic_model::serialize(peyton::io::obstream& out) const
{
	out << name();
	out << m_band << m_color;
	out << paralax_loaded << paralax;
	return out;
}

galactic_model::galactic_model(peyton::io::ibstream &in)
{
	in >> m_band >> m_color;
	in >> paralax_loaded >> paralax;
	ASSERT(in);
}

galactic_model::galactic_model(peyton::system::Config &cfg)
{
	cfg.get(m_band,  "band",   "mag");
	cfg.get(m_color, "color", "color");

	// load paralax coefficients, if there are any
	if(cfg.count("col2absmag.poly"))
	{
		std::vector<double> coeff = cfg["col2absmag.poly"];
		paralax.setParalaxCoefficients(coeff);
		paralax_loaded = true;
	}
}
