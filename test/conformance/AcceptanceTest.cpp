// Differential oracle against `tree-sitter-faust`, on acceptance and token boundaries.
// The two trees differ structurally by design.
#include "property/Corpus.h"
#include "syntax/Parser.h"

#include "doctest.h"
#include "tree_sitter/api.h"

#include <format>
#include <string>
#include <vector>

extern "C" const TSLanguage *tree_sitter_faust(void);

using namespace faustlens;
using namespace faustlens::test;

namespace {

struct TsParse {
    bool Accepted = false;
    std::vector<std::pair<uint32_t, uint32_t>> Leaves;
};

void CollectLeaves(TSNode node, std::vector<std::pair<uint32_t, uint32_t>> &out) {
    const uint32_t n = ts_node_child_count(node);
    if (n == 0) {
        if (ts_node_is_named(node)) out.emplace_back(ts_node_start_byte(node), ts_node_end_byte(node));
        return;
    }
    for (uint32_t i = 0; i < n; ++i) CollectLeaves(ts_node_child(node, i), out);
}

TsParse RunTreeSitter(TSParser *parser, const std::string &src) {
    TSTree *tree = ts_parser_parse_string(parser, nullptr, src.data(), uint32_t(src.size()));
    TsParse out;
    if (tree == nullptr) return out;
    const TSNode root = ts_tree_root_node(tree);
    out.Accepted = !ts_node_has_error(root);
    CollectLeaves(root, out.Leaves);
    ts_tree_delete(tree);
    return out;
}

} // namespace

TEST_CASE("accept/reject parity with tree-sitter-faust") {
    TSParser *parser = ts_parser_new();
    ts_parser_set_language(parser, tree_sitter_faust());

    int agree = 0;
    std::vector<std::string> we_accept_they_reject, they_accept_we_reject;
    for (const CorpusFile &f : WholeCorpus()) {
        Terms terms;
        const bool ours = Parse(terms, f.Text).Diags.empty();
        const bool theirs = RunTreeSitter(parser, f.Text).Accepted;
        if (ours == theirs) {
            ++agree;
        } else if (ours) {
            we_accept_they_reject.push_back(f.Relative);
        } else {
            they_accept_we_reject.push_back(f.Relative);
        }
    }
    ts_parser_delete(parser);

    for (const std::string &s : we_accept_they_reject) MESSAGE("we accept, ts rejects: ", s);
    for (const std::string &s : they_accept_we_reject) MESSAGE("ts accepts, we reject: ", s);
    MESSAGE("agreement on ", agree, " files");
    CHECK(we_accept_they_reject.empty());
    CHECK(they_accept_we_reject.empty());
}

TEST_CASE("leaf token boundaries agree with tree-sitter-faust") {
    // Known differences are only a coarser leaf on one side, never a boundary the other
    // lacks, so the check is nesting.
    TSParser *parser = ts_parser_new();
    ts_parser_set_language(parser, tree_sitter_faust());

    size_t files = 0, leaves = 0;
    std::vector<std::string> failures;
    for (const CorpusFile &f : WholeCorpus()) {
        Terms terms;
        if (!Parse(terms, f.Text).Diags.empty()) continue;
        const TsParse ts = RunTreeSitter(parser, f.Text);
        if (!ts.Accepted) continue;
        ++files;

        std::vector<std::pair<uint32_t, uint32_t>> ours;
        for (const Token &t : Lex(f.Text).Tokens)
            if (!IsTrivia(t.Kind) && t.Kind != Tok::Eof) ours.emplace_back(t.Begin, t.End);

        size_t i = 0;
        for (const auto &[begin, end] : ts.Leaves) {
            while (i < ours.size() && ours[i].second <= begin) ++i;
            if (i == ours.size()) break;
            // A leaf touching none of our tokens is a comment, which we lex as trivia.
            if (end <= ours[i].first) continue;
            const bool nested = (begin >= ours[i].first && end <= ours[i].second) || (begin <= ours[i].first && end >= ours[i].second);
            ++leaves;
            if (!nested) {
                failures.push_back(std::format("{}: ts leaf [{},{}) straddles our [{},{})", f.Relative, begin, end, ours[i].first, ours[i].second));
                break;
            }
        }
    }
    ts_parser_delete(parser);

    for (const std::string &s : failures) MESSAGE(s);
    MESSAGE("leaf boundaries over ", leaves, " leaves in ", files, " files");
    CHECK(failures.empty());
}
