//
// llm_filter.h - LLM candidate rerank filter
//
// Context text (rime_api get_context_text, collected by TSF frontend)
// + llama.cpp inference to rerank candidates.
// Stage 1: verify filter lifecycle and context text reading.
//
#ifndef RIME_LLM_FILTER_H_
#define RIME_LLM_FILTER_H_

#include <rime/common.h>
#include <rime/filter.h>
#include <rime/translation.h>

namespace rime {

class Context;  // 前缀软确认回调（context update_notifier）
class Engine;   // 组句候选注入需要 schema_id / context（前缀已确认文本）

// Reranked candidate stream: collect all candidates at construction,
// then yield them in reranked order.
class LlmRerankTranslation : public Translation {
 public:
  LlmRerankTranslation(an<Translation> translation,
                       const std::string& input,
                       size_t seg_start, size_t seg_end,
                       const std::string& ctx,
                       const std::string& src,
                       Engine* engine);
  bool Next() override;
  an<Candidate> Peek() override;

 private:
  void Collect();

  an<Translation> translation_;
  std::vector<an<Candidate>> candidates_;
  std::string input_;  // input code (活动段切片), for the per-inference event log
  size_t seg_start_ = 0;  // 活动段在原始 input 中的区间 [seg_start_, seg_end_)
  size_t seg_end_ = 0;    // （整句模式满覆盖 FM 判定用；单段 = [0, input 全长)）
  std::string ctx_;    // normalized context text (TSF or commit history)
  std::string src_;    // context source: "tsf" | "rime"
  Engine* engine_ = nullptr;  // N-gram 组句（schema_id / 已确认前缀）
  size_t index_ = 0;
};

class LlmFilter : public Filter {
 public:
  explicit LlmFilter(const Ticket& ticket);
  ~LlmFilter() override;

  an<Translation> Apply(an<Translation> translation,
                        CandidateList* candidates) override;

  // Text before caret (TSF collected, rime_api cached); empty = no rerank
  static std::string GetContextTextGlobal();

 private:
  // Context text + source ("tsf" | "rime"). TSF caret text wins; when TSF
  // collection never succeeded, falls back to commit history.
  std::pair<std::string, std::string> GetContextTextPair() const;
  // Commit history fallback: concat all committed texts of this session.
  // Engine thread only (Context is not thread-safe).
  std::string CommitHistoryText() const;

  // 前缀软确认执行（2026-11-23）：context update_notifier 回调。选在
  // PushInput 时（composition 仍是上一轮的、menu 已填充）执行 ctx->Select；
  // filter 阶段 seg.menu 尚未 AddTranslation（恒 null），不能在那里做。
  void OnContextUpdate(Context* ctx);
  connection update_conn_;

  // Engine commit sink: fired when text is committed to the frontend.
  // Starts async pre-decode of the upcoming context (TSF context text
  // arrives asynchronously after the commit, so we poll for the change).
  void OnCommit(const std::string& commit_text);

  connection commit_conn_;
};

}  // namespace rime

#endif  // RIME_LLM_FILTER_H_
