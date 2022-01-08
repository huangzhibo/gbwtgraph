#include <gbwtgraph/gfa.h>
#include <gbwtgraph/internal.h>

#include <algorithm>
#include <functional>
#include <limits>
#include <string>
#include <utility>

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

namespace gbwtgraph
{

//------------------------------------------------------------------------------

// Global variables.

const std::string GFA_EXTENSION = ".gfa";

// Class constants.
const std::string GFAParsingParameters::DEFAULT_REGEX = ".*";
const std::string GFAParsingParameters::DEFAULT_FIELDS = "S";

//------------------------------------------------------------------------------

// Parse a nonnegative integer, assuming that the string is valid.
std::uint64_t
stoul_unsafe(const std::string& str)
{
  std::uint64_t result = 0;
  for(char c : str) { result = 10 * result + (c - '0'); }
  return result;
}

//------------------------------------------------------------------------------

struct GFAFile
{
  // Memory mapped file.
  int    fd;
  size_t file_size;
  char*  ptr;

  // GFA information.
  bool valid_gfa;
  bool translate_segment_ids;
  size_t max_segment_length, max_path_length;

  // Bit masks for field separators.
  size_t field_end[4];
  size_t subfield_end[4];
  size_t walk_subfield_end[4];

  // Pointers to line starts.
  std::vector<const char*> s_lines;
  std::vector<const char*> l_lines;
  std::vector<const char*> p_lines;
  std::vector<const char*> w_lines;

  struct field_type
  {
    const char* begin;
    const char* end;
    size_t      line_num;
    char        type;
    bool        has_next;

    size_t size() const { return this->end - this->begin; }
    bool empty() const { return (this->size() == 0); }

    std::string str() const { return std::string(this->begin, this->end); }
    view_type view() const { return view_type(this->begin, this->end - this->begin); }
    char front() const { return *(this->begin); }
    char back() const { return *(this->end - 1); }

    // For segment orientations in links.
    bool valid_orientation() const { return (this->size() == 1 && (this->back() == '-' || this->back() == '+')); }
    bool is_reverse_orientation() const { return (this->back() == '-'); }

    // For path segment subfields.
    bool valid_path_segment() const { return (this->size() >= 2 && (this->back() == '-' || this->back() == '+')); }
    std::string path_segment() const { return std::string(this->begin, this->end - 1); }
    view_type path_segment_view() const { return view_type(this->begin, (this->end - 1) - this->begin); }
    bool is_reverse_path_segment() const { return (this->back() == '-'); }

    // Usually the next field/subfield starts at `end + 1`, because `end` points
    // to the separator. Walk subfields include the separator as a part of the field,
    // so they start at `end` instead. Before we go to the first subfield, we must
    // increment `end` (which points to the preceding '\t' before the call).
    void start_walk() { this->end++; }

    // For walk segment subfields.
    bool valid_walk_segment() const { return (this->size() >= 2 && (this->front() == '<' || this->front() == '>')); }
    std::string walk_segment() const { return std::string(this->begin + 1, this->end); }
    view_type walk_segment_view() const { return view_type(this->begin + 1, this->end - (this->begin + 1)); }
    bool is_reverse_walk_segment() const { return (this->front() == '<'); }
  };

  // Memory map and validate a GFA file. The constructor checks that all mandatory
  // fields used for GBWTGraph construction exist and are nonempty.
  // There are no checks for duplicates.
  GFAFile(const std::string& filename, bool show_progress);

  ~GFAFile();

  bool ok() const { return (this->fd >= 0 && this->file_size > 0 && this->ptr != nullptr && this->valid_gfa); }
  size_t size() const { return this->file_size; }
  size_t segments() const { return this->s_lines.size(); }
  size_t links() const { return this->l_lines.size(); }
  size_t paths() const { return this->p_lines.size(); }
  size_t walks() const { return this->w_lines.size(); }

private:
  // Preprocess a new S-line. Returns an iterator at the start of the next line or
  // nullptr if the parse failed.
  const char* add_s_line(const char* iter, size_t line_num);

  // Preprocess a new S-line. Returns an iterator at the start of the next line or
  // nullptr if the parse failed.
  const char* add_l_line(const char* iter, size_t line_num);

  // Preprocess a new P-line. Returns an iterator at the start of the next line or
  // nullptr if the parse failed.
  const char* add_p_line(const char* iter, size_t line_num);

  // Preprocess a new W-line. Returns an iterator at the start of the next line or
  // nullptr if the parse failed.
  const char* add_w_line(const char* iter, size_t line_num);

  // Returns true if the field is valid. Otherwise marks the GFA file invalid,
  // prints an error message, and returns false.
  bool check_field(const field_type& field, const std::string& field_name, bool should_have_next);

  const char* begin() const { return this->ptr; }
  const char* end() const { return this->ptr + this->size(); }

  // Return an iterator to the beginning of the next line.
  const char* next_line(const char* iter) const
  {
    while(iter != this->end() && *iter != '\n') { ++iter; }
    if(iter != this->end()) { ++iter; }
    return iter;
  }

  // Return the first tab-separated field of the line.
  field_type first_field(const char* line_start, size_t line_num = 0) const
  {
    const char* limit = line_start;
    while(limit != this->end() && !(this->is_field_end(limit))) { ++limit; }
    return { line_start, limit, line_num, *line_start, (limit != this->end() && *limit == '\t') };
  }

  // Return the next tab-separated field, assuming there is one.
  field_type next_field(const field_type& field) const
  {
    const char* limit = field.end + 1;
    while(limit != this->end() && !(this->is_field_end(limit))) { ++limit; }
    return { field.end + 1, limit, field.line_num, field.type, (limit != this->end() && *limit == '\t') };
  }

  // Return the next comma-separated subfield, assuming there is one.
  field_type next_subfield(const field_type& field) const
  {
    const char* limit = field.end + 1;
    while(limit != this->end() && !(this->is_subfield_end(limit))) { ++limit; }
    return { field.end + 1, limit, field.line_num, field.type, (limit != this->end() && *limit == ',') };
  }

  // Return the next walk subfield, assuming there is one.
  // The orientation symbol at the start of the segment is also used as subfield separator.
  field_type next_walk_subfield(const field_type& field) const
  {
    const char* limit = field.end;
    if(limit != this->end() && (*limit == '<' || *limit == '>'))
    {
      do { limit++; }
      while(limit != this->end() && !(this->is_walk_subfield_end(limit)));
    }
    return { field.end, limit, field.line_num, field.type, (limit != this->end() && (*limit == '<' || *limit == '>')) };
  }

  bool is_field_end(const char* iter) const
  {
    unsigned char c = *iter;
    return (this->field_end[c / 64] & (size_t(1) << (c & 0x3F)));
  }

  bool is_subfield_end(const char* iter) const
  {
    unsigned char c = *iter;
    return (this->subfield_end[c / 64] & (size_t(1) << (c & 0x3F)));
  }

  bool is_walk_subfield_end(const char* iter) const
  {
    unsigned char c = *iter;
    return (this->walk_subfield_end[c / 64] & (size_t(1) << (c & 0x3F)));
  }

  void add_field_end(unsigned char c)
  {
    this->field_end[c / 64] |= size_t(1) << (c & 0x3F);
  }

  void add_subfield_end(unsigned char c)
  {
    this->subfield_end[c / 64] |= size_t(1) << (c & 0x3F);
  }

  void add_walk_subfield_end(unsigned char c)
  {
    this->walk_subfield_end[c / 64] |= size_t(1) << (c & 0x3F);
  }

public:
  /*
    Iterate over the S-lines, calling segment() for all segments. Stops early if segment()
    returns false.
  */
  void for_each_segment(const std::function<bool(const std::string& name, view_type sequence)>& segment) const;

  /*
    Iterate over the L-lines, calling link() for all segments. Stops early if segment()
    returns false.
  */
 void for_each_link(const std::function<bool(const std::string& from, bool from_is_reverse, const std::string& to, bool to_is_reverse)>& link) const;

  /*
    Iterate over the file, calling path() for each path. Stops early if path() returns false.
  */
  void for_each_path_name(const std::function<bool(const std::string& name)>& path) const;

  /*
    Iterate over the file, calling path() for each path, path_segment() for
    each path segment, and finish_path() after parsing each path. Stops early
    if any call returns false.
  */
  void for_each_path(const std::function<bool(const std::string& name)>& path,
                     const std::function<bool(const std::string& name, bool is_reverse)>& path_segment,
                     const std::function<bool()>& finish_path) const;

  /*
    Iterate over the file, calling walk() for each walk. Stops early if walk() returns false.
  */
  void for_each_walk_name(const std::function<bool(const std::string& sample, const std::string& haplotype, const std::string& contig, const std::string& start)>& walk) const;

  /*
    Iterate over the file, calling walk() for each walk, walk_segment() for
    each walk segment, and finish_walk() after parsing each walk. Stops early
    if any call returns false.
  */
  void for_each_walk(const std::function<bool(const std::string& sample, const std::string& haplotype, const std::string& contig, const std::string& start)>& walk,
                     const std::function<bool(const std::string& name, bool is_reverse)>& walk_segment,
                     const std::function<bool()>& finish_walk) const;
};

//------------------------------------------------------------------------------

GFAFile::GFAFile(const std::string& filename, bool show_progress) :
  fd(-1), file_size(0), ptr(nullptr),
  valid_gfa(true), translate_segment_ids(false),
  max_segment_length(0), max_path_length(0)
{
  if(show_progress)
  {
    std::cerr << "Opening GFA file " << filename << std::endl;
  }

  // Open the file.
  this->fd = ::open(filename.c_str(), O_RDONLY);
  if(this->fd < 0)
  {
    std::cerr << "GFAFile::GFAFile(): Cannot open file " << filename << std::endl;
    return;
  }

  // Memory map the file.
  struct stat st;
  if(::fstat(this->fd, &st) < 0)
  {
    std::cerr << "GFAFile::GFAFile(): Cannot stat file " << filename << std::endl;
    return;
  }
  this->file_size = st.st_size;

  void* temp_ptr = ::mmap(nullptr, file_size, PROT_READ, MAP_FILE | MAP_SHARED, this->fd, 0);
  if(temp_ptr == MAP_FAILED)
  {
    std::cerr << "GFAFile::GFAFile(): Cannot memory map file " << filename << std::endl;
    return;
  }
  ::madvise(temp_ptr, file_size, MADV_SEQUENTIAL); // We will be making sequential passes over the data.
  this->ptr = static_cast<char*>(temp_ptr);

  // Mark characters indicating field/subfield end. This could depend on the GFA version.
  // TODO: If that happens, we need variables for field/subfield separators.
  this->field_end[0] = 0; this->field_end[1] = 0;
  this->field_end[2] = 0; this->field_end[3] = 0;
  this->add_field_end('\n'); this->add_field_end('\t');
  this->subfield_end[0] = 0; this->subfield_end[1] = 0;
  this->subfield_end[2] = 0; this->subfield_end[3] = 0;
  this->add_subfield_end('\n'); this->add_subfield_end('\t'); this->add_subfield_end(',');
  this->walk_subfield_end[0] = 0; this->walk_subfield_end[1] = 0;
  this->walk_subfield_end[2] = 0; this->walk_subfield_end[3] = 0;
  this->add_walk_subfield_end('\n'); this->add_walk_subfield_end('\t');
  this->add_walk_subfield_end('<'); this->add_walk_subfield_end('>');

  // Preprocess and validate the file.
  double start = gbwt::readTimer();
  if(show_progress)
  {
    std::cerr << "Validating GFA file " << filename << std::endl;
  }
  const char* iter = this->begin();
  size_t line_num = 0;
  while(iter != this->end())
  {
    switch(*iter)
    {
    case 'S':
      iter = this->add_s_line(iter, line_num);
      break;
    case 'L':
      iter = this->add_l_line(iter, line_num);
      break;
    case 'P':
      iter = this->add_p_line(iter, line_num);
      break;
    case 'W':
      iter = this->add_w_line(iter, line_num);
      break;
    default:
      iter = this->next_line(iter);
      break;
    }
    if(iter == nullptr) { return; }
    line_num++;
  }

  if(show_progress)
  {
    double seconds = gbwt::readTimer() - start;
    std::cerr << "Found " << this->segments() << " segments, " << this->links() << " links, " << this->paths() << " paths, and " << this->walks() << " walks in " << seconds << " seconds" << std::endl;
  }
}

const char*
GFAFile::add_s_line(const char* iter, size_t line_num)
{
  this->s_lines.push_back(iter);

  // Skip the record type field.
  field_type field = this->first_field(iter, line_num);
  if(!(this->check_field(field, "record type", true))) { return nullptr; }

  // Segment name field.
  field = this->next_field(field);
  if(!(this->check_field(field, "segment name", true))) { return nullptr; }
  std::string name = field.str();
  if(!(this->translate_segment_ids))
  {
    try
    {
      nid_t id = std::stoul(name);
      if (id == 0) { this->translate_segment_ids = true; }
    }
    catch(const std::invalid_argument&) { this->translate_segment_ids = true; }
  }

  // Sequence field.
  field = this->next_field(field);
  if(!(this->check_field(field, "sequence", false))) { return nullptr; }
  this->max_segment_length = std::max(this->max_segment_length, field.size());

  return this->next_line(field.end);
}

const char*
GFAFile::add_l_line(const char* iter, size_t line_num)
{
  this->l_lines.push_back(iter);

  // Skip the record type field.
  field_type field = this->first_field(iter, line_num);
  if(!(this->check_field(field, "record type", true))) { return nullptr; }

  // Source segment field.
  field = this->next_field(field);
  if(!(this->check_field(field, "source segment", true))) { return nullptr; }

  // Source orientation field.
  field = this->next_field(field);
  if(!(this->check_field(field, "source orientation", true))) { return nullptr; }
  if(!(field.valid_orientation()))
  {
      std::cerr << "GFAFile::add_l_line(): Invalid source orientation " << field.str() << " on line " << line_num << std::endl;
      this->valid_gfa = false;
      return nullptr;
  }

  // Destination segment field.
  field = this->next_field(field);
  if(!(this->check_field(field, "destination segment", true))) { return nullptr; }

  // Destination orientation field.
  field = this->next_field(field);
  if(!(this->check_field(field, "destination orientation", false))) { return nullptr; }
  if(!(field.valid_orientation()))
  {
      std::cerr << "GFAFile::add_l_line(): Invalid destination orientation " << field.str() << " on line " << line_num << std::endl;
      this->valid_gfa = false;
      return nullptr;
  }

  return this->next_line(field.end);
}

const char*
GFAFile::add_p_line(const char* iter, size_t line_num)
{
  this->p_lines.push_back(iter);

  // Skip the record type field.
  field_type field = this->first_field(iter, line_num);
  if(!(this->check_field(field, "record type", true))) { return nullptr; }

  // Path name field.
  field = this->next_field(field);
  if(!(this->check_field(field, "path name", true))) { return nullptr; }

  // Segment names field.
  size_t path_length = 0;
  do
  {
    field = this->next_subfield(field);
    if(!(field.valid_path_segment()))
    {
      std::cerr << "GFAFile::add_p_line(): Invalid path segment " << field.str() << " on line " << line_num << std::endl;
      this->valid_gfa = false;
      return nullptr;
    }
    path_length++;
  }
  while(field.has_next);
  if(path_length == 0)
  {
    std::cerr << "GFAFile::add_p_line(): The path on line " << line_num << " is empty" << std::endl;
    this->valid_gfa = false;
    return nullptr;
  }
  this->max_path_length = std::max(this->max_path_length, path_length);

  return this->next_line(field.end);
}

const char*
GFAFile::add_w_line(const char* iter, size_t line_num)
{
  this->w_lines.push_back(iter);

  // Skip the record type field.
  field_type field = this->first_field(iter, line_num);
  if(!(this->check_field(field, "record type", true))) { return nullptr; }

  // Sample name field.
  field = this->next_field(field);
  if(!(this->check_field(field, "sample name", true))) { return nullptr; }

  // Haplotype index field.
  field = this->next_field(field);
  if(!(this->check_field(field, "haplotype index", true))) { return nullptr; }

  // Contig name field.
  field = this->next_field(field);
  if(!(this->check_field(field, "contig name", true))) { return nullptr; }

  // Start position field.
  field = this->next_field(field);
  if(!(this->check_field(field, "start position", true))) { return nullptr; }

  // Skip the end position field.
  field = this->next_field(field);
  if(!(this->check_field(field, "end position", true))) { return nullptr; }

  // Segment names field.
  size_t path_length = 0;
  field.start_walk();
  do
  {
    field = this->next_walk_subfield(field);
    if(!(field.valid_walk_segment()))
    {
      std::cerr << "GFAFile::add_w_line(): Invalid walk segment " << field.str() << " on line " << line_num << std::endl;
      this->valid_gfa = false;
      return nullptr;
    }
    path_length++;
  }
  while(field.has_next);
  if(path_length == 0)
  {
    std::cerr << "GFAFile::add_w_line(): The walk on line " << line_num << " is empty" << std::endl;
    this->valid_gfa = false;
    return nullptr;
  }
  this->max_path_length = std::max(this->max_path_length, path_length);

  return this->next_line(field.end);
}

bool
GFAFile::check_field(const field_type& field, const std::string& field_name, bool should_have_next)
{
  if(field.empty())
  {
    std::cerr << "GFAFile::check_field(): " << field.type << "-line " << field.line_num << " has no " << field_name << std::endl;
    this->valid_gfa = false;
    return false;
  }
  if(should_have_next && !(field.has_next))
  {
    std::cerr << "GFAFile::check_field(): " << field.type << "-line " << field.line_num << " ended after " << field_name << std::endl;
    this->valid_gfa = false;
    return false;
  }
  return true;
}

GFAFile::~GFAFile()
{
  if(this->ptr != nullptr)
  {
    ::munmap(static_cast<void*>(this->ptr), this->file_size);
    this->file_size = 0;
    this->ptr = nullptr;
  }
  if(this->fd >= 0)
  {
    ::close(this->fd);
    this->fd = -1;
  }
}

//------------------------------------------------------------------------------

void
GFAFile::for_each_segment(const std::function<bool(const std::string& name, view_type sequence)>& segment) const
{
  if(!(this->ok())) { return; }

  for(const char* iter : this->s_lines)
  {
    // Skip the record type field.
    field_type field = this->first_field(iter);

    // Segment name field.
    field = this->next_field(field);
    std::string name = field.str();

    // Sequence field.
    field = this->next_field(field);
    view_type sequence = field.view();
    if(!segment(name, sequence)) { return; }
  }
}

void
GFAFile::for_each_link(const std::function<bool(const std::string& from, bool from_is_reverse, const std::string& to, bool to_is_reverse)>& link) const
{
  if(!(this->ok())) { return; }

  for(const char* iter : this->l_lines)
  {
    // Skip the record type field.
    field_type field = this->first_field(iter);

    // Source segment field.
    field = this->next_field(field);
    std::string from = field.str();

    // Source orientation field.
    field = this->next_field(field);
    bool from_is_reverse = field.is_reverse_orientation();

    // Destination segment field.
    field = this->next_field(field);
    std::string to = field.str();

    // Destination orientation field.
    field = this->next_field(field);
    bool to_is_reverse = field.is_reverse_orientation();

    if(!link(from, from_is_reverse, to, to_is_reverse)) { return; }
  }
}

void
GFAFile::for_each_path_name(const std::function<bool(const std::string& name)>& path) const
{
  if(!(this->ok())) { return; }

  for(const char* iter : this->p_lines)
  {
    // Skip the record type field.
    field_type field = this->first_field(iter);

    // Path name field.
    field = this->next_field(field);
    std::string path_name = field.str();
    if(!path(path_name)) { return; }
  }
}

void
GFAFile::for_each_path(const std::function<bool(const std::string& name)>& path,
                       const std::function<bool(const std::string& name, bool is_reverse)>& path_segment,
                       const std::function<bool()>& finish_path) const
{
  if(!(this->ok())) { return; }

  for(const char* iter : this->p_lines)
  {
    // Skip the record type field.
    field_type field = this->first_field(iter);

    // Path name field.
    field = this->next_field(field);
    if(!path(field.str())) { return; }

    // Segment names field.
    do
    {
      field = this->next_subfield(field);
      std::string segment_name = field.path_segment();
      if(!path_segment(segment_name, field.is_reverse_path_segment())) { return; }
    }
    while(field.has_next);

    if(!finish_path()) { return; }
  }
}

void
GFAFile::for_each_walk_name(const std::function<bool(const std::string& sample, const std::string& haplotype, const std::string& contig, const std::string& start)>& walk) const
{
  if(!(this->ok())) { return; }

  for(const char* iter : this->w_lines)
  {
    // Skip the record type field.
    field_type field = this->first_field(iter);

    // Sample field.
    field = this->next_field(field);
    std::string sample = field.str();

    // Haplotype field.
    field = this->next_field(field);
    std::string haplotype = field.str();

    // Contig field.
    field = this->next_field(field);
    std::string contig = field.str();

    // Start field.
    field = this->next_field(field);
    std::string start = field.str();

    if(!walk(sample, haplotype, contig, start)) { return; }
  }
}

void
GFAFile::for_each_walk(const std::function<bool(const std::string& sample, const std::string& haplotype, const std::string& contig, const std::string& start)>& walk,
                       const std::function<bool(const std::string& name, bool is_reverse)>& walk_segment,
                       const std::function<bool()>& finish_walk) const
{
  if(!(this->ok())) { return; }

  for(const char* iter : this->w_lines)
  {
    // Skip the record type field.
    field_type field = this->first_field(iter);

    // Sample field.
    field = this->next_field(field);
    std::string sample = field.str();

    // Haplotype field.
    field = this->next_field(field);
    std::string haplotype = field.str();

    // Contig field.
    field = this->next_field(field);
    std::string contig = field.str();

    // Start field.
    field = this->next_field(field);
    std::string start = field.str();

    if(!walk(sample, haplotype, contig, start)) { return; }

    // Skip the end field.
    field = this->next_field(field);

    // Segment names field.
    field.start_walk();
    do
    {
      field = this->next_walk_subfield(field);
      std::string segment_name = field.walk_segment();
      if(!walk_segment(segment_name, field.is_reverse_walk_segment())) { return; }
    }
    while(field.has_next);

    if(!finish_walk()) { return; }
  }
}

//------------------------------------------------------------------------------

bool
check_gfa_file(const GFAFile& gfa_file, const GFAParsingParameters& parameters)
{
  if(!(gfa_file.ok())) { return false; }

  if(gfa_file.segments() == 0)
  {
    std::cerr << "check_gfa_file(): No segments in the GFA file" << std::endl;
    return false;
  }
  if(gfa_file.paths() > 0 && gfa_file.walks() > 0)
  {
    if(parameters.show_progress)
    {
      std::cerr << "Storing reference paths as sample " << REFERENCE_PATH_SAMPLE_NAME << std::endl;
    }
  }
  if(gfa_file.paths() == 0 && gfa_file.walks() == 0)
  {
    std::cerr << "check_gfa_file(): No paths or walks in the GFA file" << std::endl;
    return false;
  }

  return true;
}

gbwt::size_type
determine_batch_size(const GFAFile& gfa_file, const GFAParsingParameters& parameters)
{
  gbwt::size_type batch_size = parameters.batch_size;
  if(parameters.automatic_batch_size)
  {
    gbwt::size_type min_size = gbwt::DynamicGBWT::MIN_SEQUENCES_PER_BATCH * (gfa_file.max_path_length + 1);
    batch_size = std::max(min_size, batch_size);
    batch_size = std::min(static_cast<gbwt::size_type>(gfa_file.size()), batch_size);
  }
  if(parameters.show_progress)
  {
    std::cerr << "GBWT insertion batch size: " << batch_size << " nodes" << std::endl;
  }
  return batch_size;
}

std::pair<std::unique_ptr<SequenceSource>, std::unique_ptr<EmptyGraph>>
parse_segments(const GFAFile& gfa_file, const GFAParsingParameters& parameters)
{
  double start = gbwt::readTimer();
  if(parameters.show_progress)
  {
    std::cerr << "Parsing segments" << std::endl;
  }

  // Determine if we need translation.
  bool translate = false;
  size_t max_node_length = (parameters.max_node_length == 0 ? std::numeric_limits<size_t>::max() : parameters.max_node_length);
  if(gfa_file.max_segment_length > max_node_length)
  {
    translate = true;
    if(parameters.show_progress)
    {
      std::cerr << "Breaking segments into " << max_node_length << " bp nodes" << std::endl;
    }
  }
  else if(gfa_file.translate_segment_ids)
  {
    translate = true;
    if(parameters.show_progress)
    {
      std::cerr << "Translating segment ids into valid node ids" << std::endl;
    }
  }

  std::pair<std::unique_ptr<SequenceSource>, std::unique_ptr<EmptyGraph>> result(new SequenceSource(), new EmptyGraph());
  gfa_file.for_each_segment([&](const std::string& name, view_type sequence) -> bool
  {
    if(translate)
    {
      std::pair<nid_t, nid_t> translation = result.first->translate_segment(name, sequence, max_node_length);
      for(nid_t id = translation.first; id < translation.second; id++)
      {
        result.second->create_node(id);
      }
    }
    else
    {
      nid_t id = stoul_unsafe(name);
      result.first->add_node(id, sequence);
      result.second->create_node(id);
    }
    return true;
  });

  if(parameters.show_progress)
  {
    double seconds = gbwt::readTimer() - start;
    std::cerr << "Parsed " << result.first->get_node_count() << " nodes in " << seconds << " seconds" << std::endl;
  }
  return result;
}

// FIXME parse links for GFAGraph

// FIXME this should return metadata or throw an exception
bool
parse_metadata(const GFAFile& gfa_file, const GFAParsingParameters& parameters, MetadataBuilder& metadata, gbwt::GBWTBuilder& builder)
{
  double start = gbwt::readTimer();
  if(parameters.show_progress)
  {
    std::cerr << "Parsing metadata" << std::endl;
  }
  builder.index.addMetadata();

  // Parse walks.
  if(gfa_file.walks() > 0)
  {
    // Parse reference paths.
    bool failed = false;
    if(gfa_file.paths() > 0)
    {
      gfa_file.for_each_path_name([&](const std::string& name) -> bool
      {
        if(!(metadata.add_reference_path(name))) { failed = true; return false; }
        return true;
      });
      if(failed)
      {
        std::cerr << "parse_metadata(): Could not parse GBWT metadata from reference path names" << std::endl;
        return false;
      }
    }
    // Parse walks.
    gfa_file.for_each_walk_name([&](const std::string& sample, const std::string& haplotype, const std::string& contig, const std::string& start) -> bool
    {
      if(!(metadata.add_walk(sample, haplotype, contig, start))) { failed = true; return false; }
      return true;
    });
    if(failed)
    {
      std::cerr << "parse_metadata(): Could not parse GBWT metadata from walks" << std::endl;
      return false;
    }
  }

  // Parse paths.
  else if(gfa_file.paths() > 0)
  {
    bool failed = false;
    gfa_file.for_each_path_name([&](const std::string& name) -> bool
    {
      if(!(metadata.parse(name))) { failed = true; return false; }
      return true;
    });
    if(failed)
    {
      std::cerr << "parse_metadata(): Could not parse GBWT metadata from path names" << std::endl;
      return false;
    }
  }

  builder.index.metadata = metadata.get_metadata();
  if(parameters.show_progress)
  {
    double seconds = gbwt::readTimer() - start;
    std::cerr << "Parsed metadata in " << seconds << " seconds" << std::endl;
    std::cerr << "Metadata: "; gbwt::operator<<(std::cerr, builder.index.metadata) << std::endl;
  }
  return true;
}

void
parse_paths(const GFAFile& gfa_file, const GFAParsingParameters& parameters, const SequenceSource& source, gbwt::GBWTBuilder& builder)
{
  double start = gbwt::readTimer();
  if(parameters.show_progress)
  {
    std::cerr << "Indexing paths/walks" << std::endl;
  }

  gbwt::vector_type current_path;
  auto add_segment = [&](const std::string& name, bool is_reverse) -> bool
  {
    if(source.uses_translation())
    {
      std::pair<nid_t, nid_t> range = source.get_translation(name);
      if(range.first == 0 && range.second == 0)
      {
        // FIXME error message?
        return false;
      }
      if(is_reverse)
      {
        for(nid_t id = range.second; id > range.first; id--)
        {
          current_path.push_back(gbwt::Node::encode(id - 1, is_reverse));
        }
      }
      else
      {
        for(nid_t id = range.first; id < range.second; id++)
        {
          current_path.push_back(gbwt::Node::encode(id, is_reverse));
        }
      }
    }
    else
    {
      current_path.push_back(gbwt::Node::encode(stoul_unsafe(name), is_reverse));
    }
    return true;
  };

  // Parse paths.
  gfa_file.for_each_path([&](const std::string&) -> bool
  {
    return true;
  }, add_segment, [&]() -> bool
  {
    builder.insert(current_path, true); current_path.clear();
    return true;
  });

  // Parse walks
  gfa_file.for_each_walk([&](const std::string&, const std::string&, const std::string&, const std::string&) -> bool
  {
    return true;
  }, add_segment, [&]() -> bool
  {
    builder.insert(current_path, true); current_path.clear();
    return true;
  });

  // Finish construction.
  builder.finish();
  if(parameters.show_progress)
  {
    double seconds = gbwt::readTimer() - start;
    std::cerr << "Indexed " << gfa_file.paths() << " paths and " << gfa_file.walks() << " walks in " << seconds << " seconds" << std::endl;
  }
}

//------------------------------------------------------------------------------

std::pair<std::unique_ptr<gbwt::GBWT>, std::unique_ptr<SequenceSource>>
gfa_to_gbwt(const std::string& gfa_filename, const GFAParsingParameters& parameters)
{
  // Metadata handling.
  // FIXME handle exceptions
  MetadataBuilder metadata(parameters.path_name_regex, parameters.path_name_fields);

  // GFA parsing.
  // FIXME use exceptions
  GFAFile gfa_file(gfa_filename, parameters.show_progress);
  if(!check_gfa_file(gfa_file, parameters))
  {
    return std::make_pair(std::unique_ptr<gbwt::GBWT>(nullptr), std::unique_ptr<SequenceSource>(nullptr));
  }

  // Adjust batch size by GFA size and maximum path length.
  gbwt::size_type batch_size = determine_batch_size(gfa_file, parameters);

  // Parse segments.
  std::unique_ptr<SequenceSource> source;
  std::unique_ptr<EmptyGraph> graph;
  std::tie(source, graph) = parse_segments(gfa_file, parameters);

  // FIXME add edges to graph, including ones within segments
  // FIXME determine jobs, create node-to-job mapping
  graph.reset(); // We no longer need graph topology.

  // Parse metadata from path names and walks.
  // FIXME we build and merge multiple GBWTs
  gbwt::Verbosity::set(gbwt::Verbosity::SILENT);
  gbwt::GBWTBuilder builder(parameters.node_width, batch_size, parameters.sample_interval);
  // FIXME use exceptions
  if(!parse_metadata(gfa_file, parameters, metadata, builder))
  {
    return std::make_pair(std::unique_ptr<gbwt::GBWT>(nullptr), std::unique_ptr<SequenceSource>(nullptr));
  }

  // Build GBWT from the paths and the walks.
  parse_paths(gfa_file, parameters, *source, builder);

  return std::make_pair(std::unique_ptr<gbwt::GBWT>(new gbwt::GBWT(builder.index)), std::move(source));
}

//------------------------------------------------------------------------------

// Cache segment names and lengths (in nodes). Assume that segment names are short enough
// that small string optimization avoids unnecessary memory allocations.
struct SegmentCache
{
  explicit SegmentCache(const GBWTGraph& graph) :
    graph(graph), segments((graph.index->sigma() - graph.index->firstNode()) / 2)
  {
    if(graph.has_segment_names())
    {
      graph.for_each_segment([&](const std::string& name, std::pair<nid_t, nid_t> nodes) -> bool
      {
        size_t relative = (gbwt::Node::encode(nodes.first, false) - graph.index->firstNode()) / 2;
        size_t length = nodes.second - nodes.first;
        for(size_t i = relative; i < relative + length; i++)
        {
          this->segments[i] = std::pair<size_t, size_t>(this->names.size(), length);
        }
        this->names.emplace_back(name);
        return true;
      });
    }
    else
    {
      graph.for_each_handle([&](const handle_t& handle)
      {
        size_t relative = (GBWTGraph::handle_to_node(handle) - graph.index->firstNode()) / 2;
        this->segments[relative] = std::pair<size_t, size_t>(this->names.size(), 1);
        this->names.emplace_back(std::to_string(graph.get_id(handle)));
      });
    }
  }

  size_t size() const { return this->names.size(); }

  std::pair<view_type, size_t> get(const handle_t& handle) const
  {
    return this->get(GBWTGraph::handle_to_node(handle));
  }

  std::pair<view_type, size_t> get(gbwt::node_type node) const
  {
    size_t relative = (node - this->graph.index->firstNode()) / 2;
    size_t offset = this->segments[relative].first;
    return std::make_pair(str_to_view(this->names[offset]), this->segments[relative].second);
  }

  const GBWTGraph& graph;

  // This vector goes over the same range as `graph.real_nodes`. The first component
  // is offset in `names` and the second is the length of the segment in nodes.
  std::vector<std::pair<size_t, size_t>> segments;
  std::vector<std::string> names;
};

//------------------------------------------------------------------------------

void
write_segments(const GBWTGraph& graph, const SegmentCache& cache, TSVWriter& writer, bool show_progress)
{
  double start = gbwt::readTimer();
  size_t segments = 0;
  if(show_progress)
  {
    std::cerr << "Writing segments" << std::endl;
  }

  view_type prev(nullptr, 0);
  graph.for_each_handle([&](const handle_t& handle)
  {
    auto segment = cache.get(handle);
    if(segment.first != prev)
    {
      if(prev.first != nullptr) { writer.newline(); }
      prev = segment.first;
      writer.put('S'); writer.newfield();
      writer.write(segment.first); writer.newfield();
      segments++;
    }
    writer.write(graph.get_sequence_view(handle));
  });
  if(prev.first != nullptr) { writer.newline(); }

  if(show_progress)
  {
    double seconds = gbwt::readTimer() - start;
    std::cerr << "Wrote " << segments << " segments in " << seconds << " seconds" << std::endl;
  }
}

void
write_links(const GBWTGraph& graph, const SegmentCache& cache, TSVWriter& writer, bool show_progress)
{
  double start = gbwt::readTimer();
  size_t links = 0;
  if(show_progress)
  {
    std::cerr << "Writing links" << std::endl;
  }

  if(graph.has_segment_names())
  {
    // TODO this could be faster with for_each_edge and the cache.
    graph.for_each_link([&](const edge_t& edge, const std::string& from, const std::string& to) -> bool
    {
      writer.put('L'); writer.newfield();
      writer.write(from); writer.newfield();
      writer.put((graph.get_is_reverse(edge.first) ? '-' : '+')); writer.newfield();
      writer.write(to); writer.newfield();
      writer.put((graph.get_is_reverse(edge.second) ? '-' : '+')); writer.newfield();
      writer.put('*'); writer.newline();
      links++;
      return true;
    });
  }
  else
  {
    graph.for_each_edge([&](const edge_t& edge)
    {
      writer.put('L'); writer.newfield();
      writer.write(cache.get(edge.first).first); writer.newfield();
      writer.put((graph.get_is_reverse(edge.first) ? '-' : '+')); writer.newfield();
      writer.write(cache.get(edge.second).first); writer.newfield();
      writer.put((graph.get_is_reverse(edge.second) ? '-' : '+')); writer.newfield();
      writer.put('*'); writer.newline();
      links++;
    });
  }

  if(show_progress)
  {
    double seconds = gbwt::readTimer() - start;
    std::cerr << "Wrote " << links << " links in " << seconds << " seconds" << std::endl;
  }
}

void
write_paths(const GBWTGraph& graph, const SegmentCache& cache, TSVWriter& writer, gbwt::size_type ref_sample, bool show_progress)
{
  double start = gbwt::readTimer();
  if(show_progress)
  {
    std::cerr << "Writing reference paths" << std::endl;
  }

  const gbwt::GBWT& index = *(graph.index);
  std::vector<gbwt::size_type> ref_paths = index.metadata.pathsForSample(ref_sample);
  for(gbwt::size_type path_id : ref_paths)
  {
    gbwt::vector_type path = index.extract(gbwt::Path::encode(path_id, false));
    writer.put('P'); writer.newfield();
    writer.write(index.metadata.contig(index.metadata.path(path_id).contig)); writer.newfield();
    size_t segments = 0, offset = 0;
    while(offset < path.size())
    {
      auto segment = cache.get(path[offset]);
      writer.write(segment.first);
      writer.put((gbwt::Node::is_reverse(path[offset]) ? '-' : '+'));
      segments++; offset += segment.second;
      if(offset < path.size()) { writer.put(','); }
    }
    writer.newfield();
    for(size_t i = 1; i < segments; i++)
    {
      writer.put('*');
      if(i + 1 < segments) { writer.put(','); }
    }
    writer.newline();
  }

  if(show_progress && !(ref_paths.empty()))
  {
    double seconds = gbwt::readTimer() - start;
    std::cerr << "Wrote " << ref_paths.size() << " paths in " << seconds << " seconds" << std::endl;
  }
}

void
write_walks(const GBWTGraph& graph, const SegmentCache& cache, TSVWriter& writer, gbwt::size_type ref_sample, bool show_progress)
{
  double start = gbwt::readTimer();
  size_t walks = 0;
  if(show_progress)
  {
    std::cerr << "Writing walks" << std::endl;
  }

  const gbwt::GBWT& index = *(graph.index);
  for(gbwt::size_type path_id = 0; path_id < index.metadata.paths(); path_id++)
  {
    const gbwt::PathName& path_name = index.metadata.path(path_id);
    if(path_name.sample == ref_sample) { continue; }
    walks++;
    gbwt::vector_type path = index.extract(gbwt::Path::encode(path_id, false));
    size_t length = 0;
    for(auto node : path) { length += graph.get_length(GBWTGraph::node_to_handle(node)); }
    writer.put('W'); writer.newfield();
    if(index.metadata.hasSampleNames()) { writer.write(index.metadata.sample(path_name.sample)); }
    else { writer.write(path_name.sample); }
    writer.newfield();
    writer.write(path_name.phase); writer.newfield();
    if(index.metadata.hasContigNames()) { writer.write(index.metadata.contig(path_name.contig)); }
    else { writer.write(path_name.contig); }
    writer.newfield();
    writer.write(path_name.count); writer.newfield();
    writer.write(path_name.count + length); writer.newfield();
    size_t offset = 0;
    while(offset < path.size())
    {
      auto segment = cache.get(path[offset]);
      writer.put((gbwt::Node::is_reverse(path[offset]) ? '<' : '>'));
      writer.write(segment.first);
      offset += segment.second;
    }
    writer.newline();
  }

  if(show_progress && walks > 0)
  {
    double seconds = gbwt::readTimer() - start;
    std::cerr << "Wrote " << walks << " walks in " << seconds << " seconds" << std::endl;
  }
}

void
write_all_paths(const GBWTGraph& graph, const SegmentCache& cache, TSVWriter& writer, bool show_progress)
{
  double start = gbwt::readTimer();
  if(show_progress)
  {
    std::cerr << "Writing paths" << std::endl;
  }

  const gbwt::GBWT& index = *(graph.index);
  for(gbwt::size_type seq_id = 0; seq_id < index.sequences(); seq_id += 2)
  {
    gbwt::size_type path_id = seq_id / 2;
    gbwt::vector_type path = index.extract(seq_id);
    writer.put('P'); writer.newfield();
    writer.write(std::to_string(path_id)); writer.newfield();
    size_t segments = 0, offset = 0;
    while(offset < path.size())
    {
      auto segment = cache.get(path[offset]);
      writer.write(segment.first);
      writer.put((gbwt::Node::is_reverse(path[offset]) ? '-' : '+'));
      segments++; offset += segment.second;
      if(offset < path.size()) { writer.put(','); }
    }
    writer.newfield();
    for(size_t i = 1; i < segments; i++)
    {
      writer.put('*');
      if(i + 1 < segments) { writer.put(','); }
    }
    writer.newline();
  }

  if(show_progress)
  {
    double seconds = gbwt::readTimer() - start;
    std::cerr << "Wrote " << (index.sequences() / 2) << " paths in " << seconds << " seconds" << std::endl;
  }
}

//------------------------------------------------------------------------------

void
gbwt_to_gfa(const GBWTGraph& graph, std::ostream& out, bool show_progress)
{
  bool sufficient_metadata = graph.index->hasMetadata() && graph.index->metadata.hasPathNames();

  // Cache segment names.
  double start = gbwt::readTimer();
  if(show_progress)
  {
    std::cerr << "Caching segments" << std::endl;
  }
  SegmentCache cache(graph);
  if(show_progress)
  {
    double seconds = gbwt::readTimer() - start;
    std::cerr << "Cached " << cache.size() << " segments in " << seconds << " seconds" << std::endl;
  }

  // GFA header.
  TSVWriter writer(out);
  writer.put('H'); writer.newfield();
  writer.write(std::string("VN:Z:1.0")); writer.newline();

  // Write the graph.
  write_segments(graph, cache, writer, show_progress);
  write_links(graph, cache, writer, show_progress);

  // Write the paths.
  if(sufficient_metadata)
  {
    gbwt::size_type ref_sample = graph.index->metadata.sample(REFERENCE_PATH_SAMPLE_NAME);
    write_paths(graph, cache, writer, ref_sample, show_progress);
    write_walks(graph, cache, writer, ref_sample, show_progress);
  }
  else { write_all_paths(graph, cache, writer, show_progress); }
}

//------------------------------------------------------------------------------

} // namespace gbwtgraph
