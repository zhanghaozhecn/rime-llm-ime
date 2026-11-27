//
// Copyright RIME Developers
// Distributed under the BSD License
//
// Script translator
//
// 2011-07-10 GONG Chen <chen.sst@gmail.com>
//
#include <algorithm>
#include <stack>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <boost/algorithm/string/join.hpp>
#include <boost/range/adaptor/reversed.hpp>
#include <rime/common.h>
#include <rime/composition.h>
#include <rime/candidate.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/language.h>
#include <rime/schema.h>
#include <rime/translation.h>
#include <rime/algo/syllabifier.h>
#include <rime/dict/corrector.h>
#include <rime/dict/dictionary.h>
#include <rime/dict/user_dictionary.h>
#include <rime/gear/poet.h>
#include <rime/gear/script_translator.h>
#include <rime/gear/translator_commons.h>

// static const char* quote_left = "\xef\xbc\x88";
// static const char* quote_right = "\xef\xbc\x89";

namespace rime {

// ── [账本有序源]（2026-11-27）: LLM 层用户词频账本作为第三候选源 ──
// 数据出口在 gear/llm_filter.cc（**词级聚合**：全部码形按 cum 降序 + 跨码形
// 衰减 eff），跨文件 ad-hoc 声明先例 = context.cc:19 的
// g_llm_pending_confirm_query。结构布局须与 llm_filter.cc 的定义逐字一致。
struct LedgerSrcEntry {
  string word;
  vector<string> codes;   // 该词的全部码形，按 cum 降序（最可信在前）
  double eff;             // 跨码形聚合的当前衰减 eff
};
size_t g_llm_user_source_gen();
void g_llm_user_source_entries(vector<LedgerSrcEntry>* out);

static bool user_source_on() {
  static const bool off = [] {
    const char* e = getenv("RIME_LLM_USER_SOURCE");
    return e && e[0] == '0';
  }();
  return !off;
}

namespace {

struct SyllabifyTask {
  const Code& code;
  const SyllableGraph& graph;
  size_t target_pos;
  function<void(SyllabifyTask* task,
                size_t depth,
                size_t current_pos,
                size_t next_pos)>
      push;
  function<void(SyllabifyTask* task, size_t depth)> pop;
};

static bool syllabify_dfs(SyllabifyTask* task,
                          size_t depth,
                          size_t current_pos) {
  if (depth == task->code.size()) {
    return current_pos == task->target_pos;
  }
  SyllableId syllable_id = task->code.at(depth);
  auto z = task->graph.edges.find(current_pos);
  if (z == task->graph.edges.end())
    return false;
  // favor longer spellings
  for (const auto& y : boost::adaptors::reverse(z->second)) {
    size_t end_vertex_pos = y.first;
    if (end_vertex_pos > task->target_pos)
      continue;
    auto x = y.second.find(syllable_id);
    if (x != y.second.end()) {
      task->push(task, depth, current_pos, end_vertex_pos);
      if (syllabify_dfs(task, depth + 1, end_vertex_pos))
        return true;
      task->pop(task, depth);
    }
  }
  return false;
}

}  // anonymous namespace

class ScriptSyllabifier : public PhraseSyllabifier {
 public:
  ScriptSyllabifier(ScriptTranslator* translator,
                    Corrector* corrector,
                    const string& input,
                    size_t start)
      : translator_(translator),
        input_(input),
        start_(start),
        syllabifier_(translator->delimiters(),
                     translator->enable_completion(),
                     translator->strict_spelling()) {
    if (corrector) {
      syllabifier_.EnableCorrection(corrector);
    }
  }

  virtual Spans Syllabify(const Phrase* phrase);
  size_t BuildSyllableGraph(Prism& prism);
  string GetPreeditString(const Phrase& cand) const;
  string GetOriginalSpelling(const Phrase& cand) const;
  bool IsCorrection(const Code& code, size_t code_length) const;

  const SyllableGraph& syllable_graph() const { return syllable_graph_; }

 protected:
  ScriptTranslator* translator_;
  string input_;
  size_t start_;
  Syllabifier syllabifier_;
  SyllableGraph syllable_graph_;
};

class ScriptTranslation : public Translation {
 public:
  ScriptTranslation(ScriptTranslator* translator,
                    Corrector* corrector,
                    Poet* poet,
                    const string& input,
                    size_t start,
                    size_t end_of_input,
                    int max_sentences,
                    double sentence_cutoff_threshold)
      : translator_(translator),
        poet_(poet),
        start_(start),
        end_of_input_(end_of_input),
        syllabifier_(
            New<ScriptSyllabifier>(translator, corrector, input, start)),
        enable_correction_(corrector),
        max_sentences_(max_sentences),
        sentence_cutoff_threshold_(sentence_cutoff_threshold) {
    set_exhausted(true);
  }
  bool Evaluate(Dictionary* dict, UserDictionary* user_dict);
  virtual bool Next();
  virtual an<Candidate> Peek();

 protected:
  bool CheckEmpty();
  bool IsNormalSpelling() const;
  bool PrepareCandidate();
  template <class QueryResult>
  void EnrollEntries(map<int, DictEntryList>& entries_by_end_pos,
                     const an<QueryResult>& query_result);
  WordGraph PrepareForMakingSentence(Dictionary* dict,
                                     UserDictionary* user_dict);
  an<Sentence> MakeSentence(Dictionary* dict, UserDictionary* user_dict);
  deque<an<Sentence>> MakeSentences(Dictionary* dict,
                                    UserDictionary* user_dict);

  ScriptTranslator* translator_;
  Poet* poet_;
  size_t start_;
  size_t end_of_input_;
  an<ScriptSyllabifier> syllabifier_;

  an<DictEntryCollector> phrase_;
  an<UserDictEntryCollector> user_phrase_;
  an<UserDictEntryCollector> ledger_;   // [账本有序源]（2026-11-27，RIME_LLM_USER_SOURCE=0 关）
  deque<an<Sentence>> sentences_;

  an<Phrase> candidate_ = nullptr;
  size_t candidate_index_ = 0;
  enum CandidateSource {
    kUninitialized,
    kUserPhrase,
    kSysPhrase,
    kLedger,
    kSentence,
  };
  CandidateSource candidate_source_ = kUninitialized;

  DictEntryCollector::reverse_iterator phrase_iter_;
  UserDictEntryCollector::reverse_iterator user_phrase_iter_;
  UserDictEntryCollector::reverse_iterator ledger_iter_;

  const size_t max_corrections_ = 4;
  size_t correction_count_ = 0;
  int max_sentences_ = 1;
  double sentence_cutoff_threshold_ = 0.1;

  bool enable_correction_;
};

// ScriptTranslator implementation

ScriptTranslator::ScriptTranslator(const Ticket& ticket)
    : Translator(ticket), Memory(ticket), TranslatorOptions(ticket) {
  if (!engine_)
    return;
  if (Config* config = engine_->schema()->config()) {
    config->GetInt(name_space_ + "/spelling_hints", &spelling_hints_);
    config->GetInt(name_space_ + "/max_word_length", &max_word_length_);
    config->GetInt(name_space_ + "/core_word_length", &core_word_length_);
    config->GetBool(name_space_ + "/always_show_comments",
                    &always_show_comments_);
    config->GetBool(name_space_ + "/enable_correction", &enable_correction_);
    if (!config->GetBool(name_space_ + "/enable_word_completion",
                         &enable_word_completion_)) {
      enable_word_completion_ = enable_completion_;
    }
    config->GetInt(name_space_ + "/max_homophones", &max_homophones_);
    poet_.reset(new Poet(language(), config));
  }
  if (enable_correction_) {
    if (auto* corrector = Corrector::Require("corrector")) {
      corrector_.reset(corrector->Create(ticket));
    }
  }
}

an<Translation> ScriptTranslator::Query(const string& input,
                                        const Segment& segment) {
  if (!dict_ || !dict_->loaded())
    return nullptr;
  if (!segment.HasAnyTagIn(tags_))
    return nullptr;
  DLOG(INFO) << "input = '" << input << "', [" << segment.start << ", "
             << segment.end << ")";

  FinishSession();

  bool enable_user_dict =
      user_dict_ && user_dict_->loaded() && !IsUserDictDisabledFor(input);

  size_t end_of_input = engine_->context()->input().length();
  // the translator should survive translations it creates
  auto result = New<ScriptTranslation>(
      this, corrector_.get(), poet_.get(), input, segment.start, end_of_input,
      max_sentences_, sentence_cutoff_threshold_);
  if (!result || !result->Evaluate(
                     dict_.get(), enable_user_dict ? user_dict_.get() : NULL)) {
    return nullptr;
  }
  auto deduped = New<DistinctTranslation>(result);
  if (contextual_suggestions_) {
    return poet_->ContextualWeighted(deduped, input, segment.start, this);
  }
  return deduped;
}

int ScriptTranslator::core_word_length() const {
  if (max_word_length_ <= 0) {
    return core_word_length_;
  }
  return (std::min)(core_word_length_, max_word_length_);
}

static bool exceed_upperlimit(int length, int upper_limit) {
  return upper_limit > 0 && length > upper_limit;
}

bool ScriptTranslator::SaveCommitEntry(CommitEntry& commit_entry) {
  if (exceed_upperlimit(commit_entry.Length(), max_word_length())) {
    UpdateElements(commit_entry);
  } else {
    commit_entry.Save();
  }
  return true;
}

bool ScriptTranslator::ConcatenatePhrases(CommitEntry& commit_entry,
                                          const vector<an<Phrase>>& phrases) {
  const int kCoreWordLength = core_word_length();
  const int n = phrases.size();
  for (int i = 0; i < n; ++i) {
    int cur_len = 0;
    int j = i;
    for (; j < n; ++j) {
      commit_entry.AppendPhrase(phrases.at(j));
      cur_len += phrases.at(j)->code().size();
      if (kCoreWordLength <= 0 || cur_len > kCoreWordLength) {
        break;
      }
      SaveCommitEntry(commit_entry);
    }
    if (kCoreWordLength > 0) {
      if (j == i) {
        SaveCommitEntry(commit_entry);
      }
      commit_entry.Clear();
    }
  }
  SaveCommitEntry(commit_entry);
  commit_entry.Clear();

  return true;
}

bool ScriptTranslator::ProcessSegmentOnCommit(CommitEntry& commit_entry,
                                              const Segment& seg) {
  auto phrase =
      As<Phrase>(Candidate::GetGenuineCandidate(seg.GetSelectedCandidate()));
  bool recognized = Language::intelligible(phrase, this);
  // [账本有序源]（2026-11-27）: ledger 候选照常进入提交流水线 → userdb
  // 也学习该词（entry->code 为真实音节 id，键 = 规范音节串）。与账本属
  // 双记账但服务不同消费方（userdb=引擎原生序、账本=层1/融合/有序源），
  // 且跳过学习会提前 ConcatenatePhrases 破坏部分提交的学习时机——故不跳。
  if (recognized) {
    queue_.push_back(phrase);
  }

  if (!recognized || seg.status >= Segment::kConfirmed) {
    ConcatenatePhrases(commit_entry, queue_);
    queue_.clear();
  }

  return true;
}

string ScriptTranslator::FormatPreedit(const string& preedit) {
  string result = preedit;
  preedit_formatter_.Apply(&result);
  return result;
}

string ScriptTranslator::Spell(const Code& code) {
  string result;
  vector<string> syllables;
  if (!dict_ || !dict_->Decode(code, &syllables) || syllables.empty())
    return result;
  result = boost::algorithm::join(syllables, string(1, delimiters_.at(0)));
  comment_formatter_.Apply(&result);
  return result;
}

string ScriptTranslator::GetPrecedingText(size_t start) const {
  return !contextual_suggestions_ ? string()
         : start > 0 ? engine_->context()->composition().GetTextBefore(start)
                     : engine_->context()->commit_history().latest_text();
}

bool ScriptTranslator::UpdateElements(const CommitEntry& commit_entry) {
  bool update_elements = false;
  // avoid updating single character entries within a phrase which is
  // composed with single characters only
  if (commit_entry.elements.size() > 1) {
    for (const DictEntry* e : commit_entry.elements) {
      if (e->code.size() > 1) {
        update_elements = true;
        break;
      }
    }
  }
  if (update_elements) {
    for (const DictEntry* e : commit_entry.elements) {
      user_dict_->UpdateEntry(*e, 0);
    }
  }
  return true;
}

bool ScriptTranslator::Memorize(const CommitEntry& commit_entry) {
  UpdateElements(commit_entry);
  user_dict_->UpdateEntry(commit_entry, 1);
  return true;
}

// ScriptSyllabifier implementation

Spans ScriptSyllabifier::Syllabify(const Phrase* phrase) {
  Spans result;
  vector<size_t> vertices;
  vertices.push_back(start_);
  SyllabifyTask task{
      phrase->code(), syllable_graph_, phrase->end() - start_,
      [&](SyllabifyTask* task, size_t depth, size_t current_pos,
          size_t next_pos) { vertices.push_back(start_ + next_pos); },
      [&](SyllabifyTask* task, size_t depth) { vertices.pop_back(); }};
  if (syllabify_dfs(&task, 0, phrase->start() - start_)) {
    result.set_vertices(std::move(vertices));
  }
  return result;
}

size_t ScriptSyllabifier::BuildSyllableGraph(Prism& prism) {
  return (size_t)syllabifier_.BuildSyllableGraph(input_, prism,
                                                 &syllable_graph_);
}

bool ScriptSyllabifier::IsCorrection(const Code& code,
                                     size_t code_length) const {
  vector<bool> path_attributes;
  path_attributes.reserve(8);

  SyllabifyTask task{
      code, syllable_graph_, code_length,
      // push
      [&](SyllabifyTask* task, size_t depth, size_t current_pos,
          size_t next_pos) {
        auto id = task->code[depth];

        auto start_iter = syllable_graph_.edges.find(current_pos);
        if (start_iter != syllable_graph_.edges.end()) {
          auto end_iter = start_iter->second.find(next_pos);
          if (end_iter != start_iter->second.end()) {
            auto prop_iter = end_iter->second.find(id);
            if (prop_iter != end_iter->second.end()) {
              path_attributes.push_back(prop_iter->second.is_correction);
              return;
            }
          }
        }
        // edge not found
        path_attributes.push_back(false);
      },
      // pop
      [&](SyllabifyTask* task, size_t depth) { path_attributes.pop_back(); }};

  if (syllabify_dfs(&task, 0, 0)) {
    for (bool is_correction : path_attributes) {
      if (is_correction)
        return true;
    }
  }
  return false;
}

string ScriptSyllabifier::GetPreeditString(const Phrase& cand) const {
  const auto& delimiters = translator_->delimiters();
  std::stack<size_t> lengths;
  string output;
  SyllabifyTask task{cand.matching_code(), syllable_graph_, cand.end() - start_,
                     [&](SyllabifyTask* task, size_t depth, size_t current_pos,
                         size_t next_pos) {
                       size_t len = output.length();
                       if (depth > 0 && len > 0 &&
                           delimiters.find(output[len - 1]) == string::npos) {
                         output += delimiters.at(0);
                       }
                       output +=
                           input_.substr(current_pos, next_pos - current_pos);
                       lengths.push(len);
                     },
                     [&](SyllabifyTask* task, size_t depth) {
                       output.resize(lengths.top());
                       lengths.pop();
                     }};
  if (syllabify_dfs(&task, 0, cand.start() - start_)) {
    return translator_->FormatPreedit(output);
  } else {
    return string();
  }
}

string ScriptSyllabifier::GetOriginalSpelling(const Phrase& cand) const {
  if (translator_ &&
      static_cast<int>(cand.code().size()) <= translator_->spelling_hints()) {
    return translator_->Spell(cand.code());
  }
  return string();
}

template <class Ptr, class Iter>
static bool has_exact_match_phrase(Ptr ptr, Iter iter, size_t consumed) {
  return ptr && iter->first == consumed && !iter->second.exhausted() &&
         iter->second.Peek()->IsExactMatch();
}

// ScriptTranslation implementation

// 引擎耗时探针（2026-11-23，滑窗改造定位用）：RIME_PROBE_TIMING=1 时
// 在 stderr 打印每次 Query 的分段耗时（建音节图/词典查询/造句）。
// 默认零开销（一次 getenv + 分支），不影响生产行为。
static bool probe_timing_on() {
  static const bool on = getenv("RIME_PROBE_TIMING") != nullptr;
  return on;
}
static double probe_now_ms() {
  using namespace std::chrono;
  return duration<double, std::milli>(
             steady_clock::time_point::clock::now().time_since_epoch()).count();
}

// ── [账本有序源] 查找：沿音节图匹配账本词的规范码 ──
// 镜像 UserDictionary::DfsLookup 的图走查（user_dictionary.cc:216-303），
// 但数据是内存向量（llm_filter 账本出口），匹配 = 逐音节查 indices：
//   * 全串匹配 = exact（简拼经 abbreviation 边解析到全拼音节 id —— 与
//     userdb 同款继承语义，user_dictionary.cc:237-239 首个拼写后跳过缩写）
//   * 输入末位前缀匹配（≥2 音节、耗尽输入）= predictive（completion）
// 权重域：log(eff/(eff+30)) ∈ (−∞, −0.7]——userdb/dict 权重同为 log 概率
// 域（user_dictionary.cc:554-556），exp(weight)→quality 不爆。每 end_pos
// 封顶 8 条、总封顶 64 条（仿 max_homophones/predict 上限，防候选洪泛）。
static const hash_map<string, SyllableId>& LedgerSyllabaryMap(
    const an<Table>& table) {
  static hash_map<string, SyllableId> syl2id;
  static const Table* cached = nullptr;
  if (cached != table.get()) {
    syl2id.clear();
    Syllabary syllabary;
    if (table && table->GetSyllabary(&syllabary)) {
      SyllableId id = 0;
      for (const auto& s : syllabary)
        syl2id[s] = id++;   // 字母序 == SyllableId（编译期 std::set 约定）
    }
    cached = table.get();
  }
  return syl2id;
}

// B1（2026-11-27）：账本权重标度 K（log(eff/(eff+K)) 的 K）。默认 30 =
// 「eff=1 的新词 ≈ −3.4」的原始定标；RIME_LLM_LEDGER_K 环境变量供标定扫描
// （探针 A/B），0/非法回落默认。读一次静态缓存（仿 RIME_LLM_USER_SOURCE）。
static double ledger_weight_k() {
  static double k = 0.0;
  if (k <= 0.0) {
    const char *e = std::getenv("RIME_LLM_LEDGER_K");
    double v = e ? std::atof(e) : 0.0;
    k = v > 0.0 ? v : 30.0;
  }
  return k;
}

static an<UserDictEntryCollector> BuildLedgerSource(
    ScriptTranslator* translator, const SyllableGraph& graph,
    size_t consumed) {
  static long long last_gen = -1;
  static vector<LedgerSrcEntry> entries;
  const long long gen = static_cast<long long>(g_llm_user_source_gen());
  if (gen != last_gen) {
    g_llm_user_source_entries(&entries);
    last_gen = gen;
  }
  if (entries.empty())
    return nullptr;
  const an<Table>& table = translator->dict()
                               ? translator->dict()->primary_table()
                               : an<Table>();
  if (!table)
    return nullptr;
  const auto& syl2id = LedgerSyllabaryMap(table);
  if (syl2id.empty())
    return nullptr;
  hash_map<int, DictEntryList> by_end;
  int recruited = 0;
  for (const auto& le : entries) {
    // 码形逐个尝试（2026-11-27 B2）：账本按**词**聚合，codes 按 cum 降序
    // ——取第一个能切成音节、且能沿音节图匹配的码形。旧版只取 cum 最大的
    // 那一个码形，切不开（简拼/双拼/异形码）则该词整条不入源，哪怕它有
    // 合法全拼码形。权重用**跨码形聚合的 eff**（词级总词频）。
    bool got = false;
    vector<SyllableId> path_ids, full_ids;
    size_t pos = 0, matched = 0;
    double quality_len = 0.0;
    for (const auto& code : le.codes) {
      // 规范码串 → 音节序列：带空格按空格切（userdb 键语义）；无空格 =
      // 提交段码原样（"fuqi"）→ 用 syllabary DP 切分；切不开 = 简拼/双拼/
      // 异形码 → 换下一个码形
      vector<string> syls;
      bool known = true;
      if (code.find(' ') != string::npos) {
        size_t b = 0;
        while (b <= code.size()) {
          size_t sp = code.find(' ', b);
          string syl = code.substr(b, sp == string::npos ? string::npos : sp - b);
          if (syl2id.find(syl) == syl2id.end()) {
            known = false;
            break;
          }
          syls.push_back(syl);
          if (sp == string::npos)
            break;
          b = sp + 1;
        }
      } else {
        const size_t n = code.size();
        vector<bool> reach(n + 1, false);
        vector<int> cut(n + 1, -1);
        reach[0] = true;
        for (size_t j = 1; j <= n && known; ++j) {
          for (size_t i = 0; i < j; ++i) {
            if (!reach[i])
              continue;
            if (syl2id.count(code.substr(i, j - i))) {
              reach[j] = true;
              cut[j] = static_cast<int>(i);
              break;
            }
          }
        }
        if (!reach[n]) {
          known = false;
        } else {
          for (size_t j = n; j > 0;) {
            int i = cut[j];
            syls.push_back(code.substr(static_cast<size_t>(i), j - i));
            j = static_cast<size_t>(i);
          }
          std::reverse(syls.begin(), syls.end());
        }
      }
      if (!known || syls.empty())
        continue;
      // 沿音节图逐音节匹配；断点后仅补全尾段 id（predictive 用）
      path_ids.clear();
      full_ids.clear();
      pos = 0;
      matched = 0;
      quality_len = 0.0;
      for (size_t i = 0; i < syls.size(); ++i) {
        const SyllableId id = syl2id.at(syls[i]);
        full_ids.push_back(id);
        if (i != matched)
          continue;
        auto iit = graph.indices.find(pos);
        if (iit == graph.indices.end())
          continue;   // 图耗尽 → matched 停在此
        auto spit = iit->second.find(id);
        if (spit == iit->second.end())
          continue;
        // 取首个拼写属性（缩写变体跳过，镜像 user_dictionary.cc:237-239）
        const EdgeProperties* props =
            spit->second.empty() ? nullptr : spit->second.front();
        if (!props)
          continue;
        path_ids.push_back(id);
        if (props->type == kNormalSpelling)
          quality_len += static_cast<double>(props->end_pos - pos);
        pos = props->end_pos;
        ++matched;
      }
      const bool exact = matched == syls.size();
      const bool predictive =
          !exact && matched >= 2 && pos == consumed && matched < syls.size();
      if (exact || predictive) {
        got = true;
        break;
      }
    }
    if (!got)
      continue;
    const bool exact = matched == full_ids.size();
    auto e = New<DictEntry>();
    e->text = le.word;
    // B1 标定钩子（2026-11-27）：K 默认 30，可用 RIME_LLM_LEDGER_K 覆盖做
    // 权重标度扫描（选重率数据重估）；0/非法值回落默认。权重语义 =
    // log(eff/(eff+K)) ∈ log 概率域，与 sys 侧 log(count/1e8)+credibility 可比。
    e->weight = std::log(le.eff / (le.eff + ledger_weight_k()));
    e->quality_len = quality_len;
    e->commit_count = 0;
    if (exact) {
      e->code = Code(path_ids.begin(), path_ids.end());
      e->matching_code_size = path_ids.size();   // == code.size() → exact
    } else {
      e->code = Code(full_ids.begin(), full_ids.end());
      e->matching_code_size = matched;           // < code.size() → predictive
    }
    by_end[static_cast<int>(pos)].push_back(e);
    if (++recruited >= 64)
      break;
  }
  if (by_end.empty())
    return nullptr;
  auto result = New<UserDictEntryCollector>();
  for (auto& x : by_end) {
    auto& list = x.second;
    std::stable_sort(list.begin(), list.end(),
                     [](const an<DictEntry>& a, const an<DictEntry>& b) {
                       return a->weight > b->weight;
                     });
    // 精确匹配置首（镜像 user_dictionary.cc:337-354 预测模式 rotate 语义）
    for (size_t i = 0; i < list.size(); ++i) {
      if (list[i]->IsExactMatch() && i > 0) {
        auto ex = list[i];
        list.erase(list.begin() + i);
        list.insert(list.begin(), ex);
        break;
      }
    }
    if (list.size() > 8)
      list.resize(8);
    (*result)[x.first].SetEntries(std::move(list));
  }
  return result;
}

bool ScriptTranslation::Evaluate(Dictionary* dict, UserDictionary* user_dict) {
  const bool tm = probe_timing_on();
  const double t0 = tm ? probe_now_ms() : 0.0;
  size_t consumed = syllabifier_->BuildSyllableGraph(*dict->prism());
  const auto& syllable_graph = syllabifier_->syllable_graph();
  const double t1 = tm ? probe_now_ms() : 0.0;
  bool predict_word = translator_->enable_word_completion() &&
                      start_ + consumed == end_of_input_;

  phrase_ =
      dict->Lookup(syllable_graph, 0, &translator_->blacklist(), predict_word);
  if (user_dict) {
    const size_t kUnlimitedDepth = 0;
    const size_t kNumSyllablesToPredictWord = 4;
    user_phrase_ =
        user_dict->Lookup(syllable_graph, 0, kUnlimitedDepth,
                          predict_word ? kNumSyllablesToPredictWord : 0);
  }
  // [账本有序源]（2026-11-27）：LLM 用户词频账本作为第三候选源。
  // 简拼经音节图 abbreviation 边解析到全拼音节 id —— 与 userdb 同款
  // 「简拼继承全拼词频」语义。A/B：RIME_LLM_USER_SOURCE=0 关。
  if (user_source_on())
    ledger_ = BuildLedgerSource(translator_, syllable_graph, consumed);
  else
    ledger_ = nullptr;
  const double t2 = tm ? probe_now_ms() : 0.0;
  if (!phrase_ && !user_phrase_ && !ledger_)
    return false;

  if (phrase_)
    phrase_iter_ = phrase_->rbegin();
  if (user_phrase_)
    user_phrase_iter_ = user_phrase_->rbegin();
  if (ledger_)
    ledger_iter_ = ledger_->rbegin();

  auto is_correction_match = [&](auto& iter_pair, size_t len) {
    if (iter_pair.first != len || iter_pair.second.exhausted())
      return false;
    return syllabifier_->IsCorrection(iter_pair.second.Peek()->code, len);
  };

  bool has_reliable_phrase =
      has_exact_match_phrase(phrase_, phrase_iter_, consumed) &&
      !is_correction_match(*phrase_iter_, consumed);

  bool has_reliable_user_phrase =
      has_exact_match_phrase(user_phrase_, user_phrase_iter_, consumed) &&
      !is_correction_match(*user_phrase_iter_, consumed);

  bool has_reliable_ledger =
      has_exact_match_phrase(ledger_, ledger_iter_, consumed);

  bool has_at_least_two_syllables = syllable_graph.edges.size() >= 2;
  DLOG(INFO) << "consumed: " << consumed
             << ", has_reliable_phrase: " << has_reliable_phrase
             << ", has_reliable_user_phrase: " << has_reliable_user_phrase
             << ", has_reliable_ledger: " << has_reliable_ledger
             << ", has_at_least_two_syllables: " << has_at_least_two_syllables;
  // make sentences when there is no exact-matching phrase candidate
  // （账本整段精确命中也抑制造句——词级整码命中意味着用户意图是词而非句；
  //   整句长码下账本几乎无整句条目 → 不影响组句）
  if (has_at_least_two_syllables && !has_reliable_phrase &&
      !has_reliable_user_phrase && !has_reliable_ledger) {
    if (max_sentences_ > 1)
      sentences_ = MakeSentences(dict, user_dict);
    else if (max_sentences_) {
      auto sentence = MakeSentence(dict, user_dict);
      if (sentence)
        sentences_ = {sentence};
      else
        sentences_.clear();
    }
  }

  const double t3 = tm ? probe_now_ms() : 0.0;
  if (tm) {
    fprintf(stderr,
            "[timing] buildgraph=%.2f lookup=%.2f sentence=%.2f groups=%zu/%zu\n",
            t1 - t0, t2 - t1, t3 - t2,
            phrase_ ? phrase_->size() : 0,
            user_phrase_ ? user_phrase_->size() : 0);
  }

  return !CheckEmpty();
}

bool ScriptTranslation::Next() {
  do {
    if (exhausted())
      return false;
    if (candidate_source_ == kUninitialized) {
      PrepareCandidate();  // to determine candidate_source_
    }
    switch (candidate_source_) {
      case kUninitialized:
        break;
      case kSentence:
        if (!sentences_.empty())
          sentences_.pop_front();
        break;
      case kUserPhrase: {
        UserDictEntryIterator& uter(user_phrase_iter_->second);
        if (!uter.Next()) {
          ++user_phrase_iter_;
        }
      } break;
      case kSysPhrase: {
        DictEntryIterator& iter(phrase_iter_->second);
        if (!iter.Next()) {
          ++phrase_iter_;
        }
      } break;
      case kLedger: {
        UserDictEntryIterator& liter(ledger_iter_->second);
        if (!liter.Next()) {
          ++ledger_iter_;
        }
      } break;
    }
    candidate_.reset();
    candidate_source_ = kUninitialized;
    if (enable_correction_) {
      // populate next candidate and skip it if it's a correction beyond max
      // numbers.
      if (!PrepareCandidate()) {
        break;
      }
    }
  } while (enable_correction_ &&
           syllabifier_->IsCorrection(candidate_->code(),
                                      candidate_->end() - start_) &&
           // limit the number of correction candidates
           ++correction_count_ > max_corrections_);
  if (!CheckEmpty()) {
    ++candidate_index_;
    return true;
  }
  return false;
}

an<Candidate> ScriptTranslation::Peek() {
  if (candidate_source_ == kUninitialized && !PrepareCandidate()) {
    return nullptr;
  }
  if (candidate_->preedit().empty()) {
    candidate_->set_preedit(syllabifier_->GetPreeditString(*candidate_));
  }
  if (candidate_->comment().empty()) {
    auto spelling = syllabifier_->GetOriginalSpelling(*candidate_);
    if (!spelling.empty() && (translator_->always_show_comments() ||
                              spelling != candidate_->preedit())) {
      candidate_->set_comment(/*quote_left + */ spelling /* + quote_right*/);
    }
  }
  candidate_->set_syllabifier(syllabifier_);
  return candidate_;
}

static bool always_true() {
  return true;
}

template <typename T>
inline static bool prefer_user_phrase(
    T user_phrase_weight,
    T sys_phrase_weight,
    function<bool()> compare_on_tie = always_true) {
  return user_phrase_weight > sys_phrase_weight ||
         (user_phrase_weight == sys_phrase_weight && compare_on_tie());
}

bool ScriptTranslation::PrepareCandidate() {
iter_incremented:
  if (exhausted()) {
    candidate_source_ = kUninitialized;
    candidate_ = nullptr;
    return false;
  }
  if (!sentences_.empty()) {
    candidate_source_ = kSentence;
    candidate_ = sentences_[0];
    return true;
  }
  const size_t full_code_length = end_of_input_ - start_;
  size_t user_phrase_code_length = 0;
  if (user_phrase_ && user_phrase_iter_ != user_phrase_->rend()) {
    user_phrase_code_length = user_phrase_iter_->first;
  }
  size_t phrase_code_length = 0;
  if (phrase_ && phrase_iter_ != phrase_->rend()) {
    phrase_code_length = phrase_iter_->first;
  }
  // [账本有序源] 组迭代器耗尽时顺移（镜像 sys exhausted-skip），再取长度
  while (ledger_ && ledger_iter_ != ledger_->rend() &&
         ledger_iter_->second.exhausted()) {
    ++ledger_iter_;
  }
  size_t ledger_code_length = 0;
  if (ledger_ && ledger_iter_ != ledger_->rend()) {
    ledger_code_length = ledger_iter_->first;
  }
  // [账本有序源] 三源比较（2026-11-27）：保持既有 user-vs-sys 全部语义
  // （纠错让权 / kNumExactMatchOnTop），ledger 只在两种情况下接管——
  //   ① 严格更长（码长优先规则）
  //   ② 与 sys 同长且 user 不覆盖该长度（用户词频 > 系统词，镜像
  //      userdb>sys 同长优先；与 user 同长时 userdb 原生学习优先）
  // 并且（2026-11-27 用户定案）**同 span 时首位不许联想压精确**：镜像
  // user-vs-sys 的 kNumExactMatchOnTop=1——「长词联想之前须至少出一个严格
  // 匹配的候选」。缺这条时账本里一次输入的新词（eff≈1 → log(eff/(eff+30))
  // ≈−3.4）就可能以联想身份压掉同 span 的系统精确词。
  // 只在「与 sys 同 span」这一支生效：ledger 严格更长时该 span 上没有别的
  // 源可比，span 优先（先长后短）照旧——原生也是 span 绝对优先。
  // B1（2026-11-27）：同 span 接管改为**权重比较**——账本词权重必须严格高于
  // sys 组首（两者同为 log 概率域：账本 log(eff/(eff+30))、sys log(count/1e8)
  // + credibility），否则让位给系统词、排队到该组之后再比。旧版无条件接管
  // 会让只打过一次的词（eff=1）整组压过高频系统词。
  const bool ledger_live =
      ledger_ && ledger_iter_ != ledger_->rend() && ledger_code_length > 0;
  const bool ledger_span_wins =
      ledger_live &&
      (ledger_code_length > std::max(user_phrase_code_length, phrase_code_length) ||
       (ledger_code_length == phrase_code_length &&
        ledger_code_length > user_phrase_code_length));
  const bool ledger_tie_with_sys =
      ledger_code_length == phrase_code_length &&
      ledger_code_length > user_phrase_code_length;
  if (ledger_span_wins) {
    UserDictEntryIterator& liter = ledger_iter_->second;
    const auto& entry = liter.Peek();
    bool ok_here = true;
    if (ledger_tie_with_sys) {
      // 精确优先（联想不许占首位）；同 span 还要比权重
      if (entry->IsPredictiveMatch() && candidate_index_ == 0)
        ok_here = false;
      else {
        double sys_w = -1e308;
        if (phrase_iter_ != phrase_->rend())
          sys_w = phrase_iter_->second.Peek()->weight;
        if (!(entry->weight > sys_w))
          ok_here = false;
      }
    }
    if (ok_here) {
      DLOG(INFO) << "ledger phrase '" << entry->text
                 << "', code length: " << ledger_code_length;
      candidate_source_ = kLedger;
      candidate_ =
          New<Phrase>(translator_->language(),
                      entry->IsPredictiveMatch() ? "completion" : "ledger",
                      start_, start_ + ledger_code_length, entry);
      candidate_->set_quality(std::exp(entry->weight) +
                              translator_->initial_quality() +
                              (entry->quality_len / full_code_length));
      return true;
    }
  }
  if (user_phrase_code_length > 0 &&
      prefer_user_phrase(
          user_phrase_code_length, phrase_code_length,
          // 編碼長度相同時, 用戶詞優先
          [this, full_code_length, phrase_code_length]() {
            UserDictEntryIterator& uter = user_phrase_iter_->second;
            DictEntryIterator& iter = phrase_iter_->second;
            // 但若用戶詞爲糾錯結果, 則免去優先權, 回歸權重比較
            bool user_is_correction = syllabifier_->IsCorrection(
                uter.Peek()->code, phrase_code_length);
            if (user_is_correction) {
              // 因對糾錯候選施加了較大懲罰, 通常結果爲 系統原文 > 用戶糾錯
              // 兩者皆糾錯時, 用戶詞的動態權重有助達成 用戶糾錯 > 系統糾錯
              return uter.Peek()->weight >= iter.Peek()->weight;
            }
            // 長詞聯想之前須至少出一個嚴格匹配的候選
            // 故確定首選之際, 系統嚴格匹配 > 用戶長詞聯想
            const int kNumExactMatchOnTop = 1;
            return candidate_index_ >= kNumExactMatchOnTop ||
                   prefer_user_phrase(
                       has_exact_match_phrase(user_phrase_, user_phrase_iter_,
                                              full_code_length),
                       has_exact_match_phrase(phrase_, phrase_iter_,
                                              full_code_length));
          })) {
    UserDictEntryIterator& uter = user_phrase_iter_->second;
    const auto& entry = uter.Peek();
    DLOG(INFO) << "user phrase '" << entry->text
               << "', code length: " << user_phrase_code_length;
    candidate_source_ = kUserPhrase;
    candidate_ =
        New<Phrase>(translator_->language(),
                    entry->IsPredictiveMatch() ? "completion" : "user_phrase",
                    start_, start_ + user_phrase_code_length, entry);
    candidate_->set_quality(std::exp(entry->weight) +
                            translator_->initial_quality() +
                            (entry->quality_len / full_code_length));
    return true;
  } else if (phrase_code_length > 0) {
    DictEntryIterator& iter = phrase_iter_->second;
    if (iter.exhausted()) {
      ++phrase_iter_;
      goto iter_incremented;
    }
    const auto& entry = iter.Peek();
    DLOG(INFO) << "phrase '" << entry->text
               << "', code length: " << phrase_code_length;
    candidate_source_ = kSysPhrase;
    candidate_ =
        New<Phrase>(translator_->language(),
                    entry->IsPredictiveMatch() ? "completion" : "phrase",
                    start_, start_ + phrase_code_length, entry);
    candidate_->set_quality(std::exp(entry->weight) +
                            translator_->initial_quality() +
                            (entry->quality_len / full_code_length));
    return true;
  } else {
    candidate_source_ = kUninitialized;
    candidate_ = nullptr;
    return false;
  }
}

bool ScriptTranslation::CheckEmpty() {
  set_exhausted((!phrase_ || phrase_iter_ == phrase_->rend()) &&
                (!user_phrase_ || user_phrase_iter_ == user_phrase_->rend()) &&
                (!ledger_ || ledger_iter_ == ledger_->rend()));
  return exhausted();
}

template <class QueryResult>
void ScriptTranslation::EnrollEntries(
    map<int, DictEntryList>& entries_by_end_pos,
    const an<QueryResult>& query_result) {
  if (query_result) {
    for (auto& y : *query_result) {
      DictEntryList& homophones = entries_by_end_pos[y.first];
      while (homophones.size() < translator_->max_homophones() &&
             !y.second.exhausted()) {
        homophones.push_back(y.second.Peek());
        if (!y.second.Next())
          break;
      }
    }
  }
}

WordGraph ScriptTranslation::PrepareForMakingSentence(
    Dictionary* dict,
    UserDictionary* user_dict) {
  const int kMaxSyllablesForUserPhraseQuery = 5;
  const auto& syllable_graph = syllabifier_->syllable_graph();
  WordGraph graph;
  for (const auto& x : syllable_graph.edges) {
    auto& same_start_pos = graph[x.first];
    if (user_dict) {
      EnrollEntries(same_start_pos,
                    user_dict->Lookup(syllable_graph, x.first,
                                      kMaxSyllablesForUserPhraseQuery));
    }
    // merge lookup results
    EnrollEntries(same_start_pos, dict->Lookup(syllable_graph, x.first,
                                               &translator_->blacklist()));
  }
  return graph;
}

// 滑窗组句（2026-11-23，千问 seg_window_syllables 思路）：WordGraph 的 key
// = 音节边界位置（升序）。窗口 W>0 时只在前 W 个音节位置内组句（前缀整句），
// 控制长拼写串的组句规模；W<=0 或位置不足 → 原 total_length（行为不变）。
static size_t EffectiveSentenceLength(const WordGraph& graph,
                                      size_t total_length, int window) {
  if (window <= 0)
    return total_length;
  size_t n = 0;
  for (const auto& sv : graph) {
    if (++n > static_cast<size_t>(window))
      return sv.first;  // 第 W+1 个位置 = 窗口末端
  }
  return total_length;
}

deque<an<Sentence>> ScriptTranslation::MakeSentences(
    Dictionary* dict,
    UserDictionary* user_dict) {
  const auto& syllable_graph = syllabifier_->syllable_graph();
  WordGraph graph = PrepareForMakingSentence(dict, user_dict);
  const size_t total_len = EffectiveSentenceLength(
      graph, syllable_graph.interpreted_length,
      translator_->seg_window_syllables());
  auto sentences =
      poet_->MakeSentences(graph, total_len,
                           translator_->GetPrecedingText(start_),
                           max_sentences_, sentence_cutoff_threshold_);
  for (auto& sentence : sentences) {
    sentence->Offset(start_);
    sentence->set_syllabifier(syllabifier_);
  }
  return sentences;
}

an<Sentence> ScriptTranslation::MakeSentence(Dictionary* dict,
                                             UserDictionary* user_dict) {
  const auto& syllable_graph = syllabifier_->syllable_graph();
  WordGraph graph = PrepareForMakingSentence(dict, user_dict);
  const size_t total_len = EffectiveSentenceLength(
      graph, syllable_graph.interpreted_length,
      translator_->seg_window_syllables());
  if (auto sentence =
          poet_->MakeSentence(graph, total_len,
                              translator_->GetPrecedingText(start_))) {
    sentence->Offset(start_);
    sentence->set_syllabifier(syllabifier_);
    return sentence;
  }
  return nullptr;
}

}  // namespace rime
