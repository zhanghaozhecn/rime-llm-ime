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

// Reranked candidate stream: collect all candidates at construction,
// then yield them in reranked order.
class LlmRerankTranslation : public Translation {
 public:
  LlmRerankTranslation(an<Translation> translation,
                       const std::string& input,
                       const std::string& ctx,
                       const std::string& src);
  bool Next() override;
  an<Candidate> Peek() override;

 private:
  void Collect();

  an<Translation> translation_;
  std::vector<an<Candidate>> candidates_;
  std::string input_;  // input code, for the per-inference event log
  std::string ctx_;    // normalized context text (TSF or commit history)
  std::string src_;    // context source: "tsf" | "rime"
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

  // Engine commit sink: fired when text is committed to the frontend.
  // Starts async pre-decode of the upcoming context (TSF context text
  // arrives asynchronously after the commit, so we poll for the change).
  void OnCommit(const std::string& commit_text);

  connection commit_conn_;
};

}  // namespace rime

#endif  // RIME_LLM_FILTER_H_
