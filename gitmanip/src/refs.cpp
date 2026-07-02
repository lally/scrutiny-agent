#include "gitmanip/refs.hpp"
#include "gitmanip/commit.hpp"
#include "gitmanip/error.hpp"
#include "gitmanip/repository.hpp"
#include "gitmanip/signing.hpp"

#include <git2.h>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

namespace gitmanip {

// =============================================================================
// Remote implementation
// =============================================================================

std::vector<RemoteInfo> Remote::list(Repository& repo) {
    std::vector<RemoteInfo> result;

    git_strarray remote_names = {nullptr, 0};
    int error = git_remote_list(&remote_names, repo.raw());
    detail::check_libgit2_error(error, "listing remotes");

    for (size_t i = 0; i < remote_names.count; ++i) {
        auto info = get(repo, remote_names.strings[i]);
        if (info) {
            result.push_back(std::move(*info));
        }
    }

    git_strarray_dispose(&remote_names);
    return result;
}

std::optional<RemoteInfo> Remote::get(Repository& repo, std::string_view name) {
    git_remote* remote = nullptr;
    int error = git_remote_lookup(&remote, repo.raw(), std::string(name).c_str());

    if (error == GIT_ENOTFOUND) {
        return std::nullopt;
    }
    detail::check_libgit2_error(error, fmt::format("looking up remote {}", name));

    RemoteInfo info;
    info.name = std::string(name);

    const char* url = git_remote_url(remote);
    info.url = url ? url : "";

    const char* push_url = git_remote_pushurl(remote);
    info.push_url = push_url ? push_url : info.url;

    // Get fetch refspecs
    const git_refspec* refspec;
    size_t refspec_count = git_remote_refspec_count(remote);
    for (size_t i = 0; i < refspec_count; ++i) {
        refspec = git_remote_get_refspec(remote, i);
        if (refspec) {
            const char* str = git_refspec_string(refspec);
            if (str) {
                if (git_refspec_direction(refspec) == GIT_DIRECTION_FETCH) {
                    info.fetch_refspecs.push_back(str);
                } else {
                    info.push_refspecs.push_back(str);
                }
            }
        }
    }

    git_remote_free(remote);
    return info;
}

std::vector<std::string> Remote::names(Repository& repo) {
    std::vector<std::string> result;

    git_strarray remote_names = {nullptr, 0};
    int error = git_remote_list(&remote_names, repo.raw());
    detail::check_libgit2_error(error, "listing remote names");

    for (size_t i = 0; i < remote_names.count; ++i) {
        result.emplace_back(remote_names.strings[i]);
    }

    git_strarray_dispose(&remote_names);
    return result;
}

bool Remote::exists(Repository& repo, std::string_view name) {
    git_remote* remote = nullptr;
    int error = git_remote_lookup(&remote, repo.raw(), std::string(name).c_str());

    if (error == GIT_ENOTFOUND) {
        return false;
    }
    if (error < 0) {
        return false;
    }

    git_remote_free(remote);
    return true;
}

void Remote::add(Repository& repo, std::string_view name, std::string_view url) {
    git_remote* remote = nullptr;
    int error = git_remote_create(&remote, repo.raw(),
                                  std::string(name).c_str(),
                                  std::string(url).c_str());
    detail::check_libgit2_error(error, fmt::format("creating remote {}", name));
    git_remote_free(remote);
}

void Remote::remove(Repository& repo, std::string_view name) {
    int error = git_remote_delete(repo.raw(), std::string(name).c_str());
    detail::check_libgit2_error(error, fmt::format("deleting remote {}", name));
}

void Remote::rename(Repository& repo, std::string_view old_name, std::string_view new_name) {
    git_strarray problems = {nullptr, 0};
    int error = git_remote_rename(&problems, repo.raw(),
                                  std::string(old_name).c_str(),
                                  std::string(new_name).c_str());
    git_strarray_dispose(&problems);
    detail::check_libgit2_error(error, fmt::format("renaming remote {} to {}", old_name, new_name));
}

void Remote::set_url(Repository& repo, std::string_view name, std::string_view url) {
    int error = git_remote_set_url(repo.raw(),
                                   std::string(name).c_str(),
                                   std::string(url).c_str());
    detail::check_libgit2_error(error, fmt::format("setting URL for remote {}", name));
}

void Remote::set_push_url(Repository& repo, std::string_view name, std::string_view url) {
    int error = git_remote_set_pushurl(repo.raw(),
                                       std::string(name).c_str(),
                                       std::string(url).c_str());
    detail::check_libgit2_error(error, fmt::format("setting push URL for remote {}", name));
}

// =============================================================================
// Branch implementation
// =============================================================================

std::vector<BranchInfo> Branch::list(Repository& repo, bool local, bool remote) {
    std::vector<BranchInfo> result;

    git_branch_t filter = static_cast<git_branch_t>(0);
    if (local) filter = static_cast<git_branch_t>(filter | GIT_BRANCH_LOCAL);
    if (remote) filter = static_cast<git_branch_t>(filter | GIT_BRANCH_REMOTE);

    if (filter == 0) return result;

    git_branch_iterator* iter = nullptr;
    int error = git_branch_iterator_new(&iter, repo.raw(), filter);
    detail::check_libgit2_error(error, "creating branch iterator");

    // Get current HEAD for is_head check
    git_reference* head_ref = nullptr;
    Oid head_target;
    bool has_head = git_repository_head(&head_ref, repo.raw()) == 0;
    if (has_head && head_ref) {
        const git_oid* head_oid = git_reference_target(head_ref);
        if (head_oid) {
            head_target = Oid(head_oid);
        }
        git_reference_free(head_ref);
    }

    git_reference* ref = nullptr;
    git_branch_t branch_type;

    while (git_branch_next(&ref, &branch_type, iter) == 0) {
        BranchInfo info;

        const char* name = nullptr;
        git_branch_name(&name, ref);
        info.name = name ? name : "";
        info.refname = git_reference_name(ref);
        info.is_remote = (branch_type == GIT_BRANCH_REMOTE);

        // Get target
        const git_oid* target = git_reference_target(ref);
        if (target) {
            info.target = Oid(target);
        }

        // Check if this is HEAD
        info.is_head = git_branch_is_head(ref) != 0;

        // Get upstream for local branches
        if (!info.is_remote) {
            git_reference* upstream_ref = nullptr;
            if (git_branch_upstream(&upstream_ref, ref) == 0 && upstream_ref) {
                const char* upstream_name = nullptr;
                git_branch_name(&upstream_name, upstream_ref);
                if (upstream_name) {
                    info.upstream = std::string(upstream_name);
                }
                git_reference_free(upstream_ref);
            }
        } else {
            // Extract remote name for remote branches
            size_t slash_pos = info.name.find('/');
            if (slash_pos != std::string::npos) {
                info.remote_name = info.name.substr(0, slash_pos);
            }
        }

        result.push_back(std::move(info));
        git_reference_free(ref);
    }

    git_branch_iterator_free(iter);
    return result;
}

std::optional<BranchInfo> Branch::get(Repository& repo, std::string_view name, bool is_remote) {
    git_reference* ref = nullptr;
    git_branch_t branch_type = is_remote ? GIT_BRANCH_REMOTE : GIT_BRANCH_LOCAL;

    int error = git_branch_lookup(&ref, repo.raw(), std::string(name).c_str(), branch_type);
    if (error == GIT_ENOTFOUND) {
        return std::nullopt;
    }
    detail::check_libgit2_error(error, fmt::format("looking up branch {}", name));

    BranchInfo info;
    info.name = std::string(name);
    info.refname = git_reference_name(ref);
    info.is_remote = is_remote;

    const git_oid* target = git_reference_target(ref);
    if (target) {
        info.target = Oid(target);
    }

    info.is_head = git_branch_is_head(ref) != 0;

    if (!is_remote) {
        git_reference* upstream_ref = nullptr;
        if (git_branch_upstream(&upstream_ref, ref) == 0 && upstream_ref) {
            const char* upstream_name = nullptr;
            git_branch_name(&upstream_name, upstream_ref);
            if (upstream_name) {
                info.upstream = std::string(upstream_name);
            }
            git_reference_free(upstream_ref);
        }
    } else {
        size_t slash_pos = info.name.find('/');
        if (slash_pos != std::string::npos) {
            info.remote_name = info.name.substr(0, slash_pos);
        }
    }

    git_reference_free(ref);
    return info;
}

std::optional<BranchInfo> Branch::current(Repository& repo) {
    if (repo.is_head_detached()) {
        return std::nullopt;
    }

    git_reference* head_ref = nullptr;
    int error = git_repository_head(&head_ref, repo.raw());
    if (error != 0) {
        return std::nullopt;
    }

    const char* name = nullptr;
    git_branch_name(&name, head_ref);

    BranchInfo info;
    info.name = name ? name : "";
    info.refname = git_reference_name(head_ref);
    info.is_remote = false;
    info.is_head = true;

    const git_oid* target = git_reference_target(head_ref);
    if (target) {
        info.target = Oid(target);
    }

    // Get upstream
    git_reference* upstream_ref = nullptr;
    if (git_branch_upstream(&upstream_ref, head_ref) == 0 && upstream_ref) {
        const char* upstream_name = nullptr;
        git_branch_name(&upstream_name, upstream_ref);
        if (upstream_name) {
            info.upstream = std::string(upstream_name);
        }
        git_reference_free(upstream_ref);
    }

    git_reference_free(head_ref);
    return info;
}

bool Branch::exists(Repository& repo, std::string_view name, bool is_remote) {
    git_reference* ref = nullptr;
    git_branch_t branch_type = is_remote ? GIT_BRANCH_REMOTE : GIT_BRANCH_LOCAL;

    int error = git_branch_lookup(&ref, repo.raw(), std::string(name).c_str(), branch_type);
    if (error == GIT_ENOTFOUND) {
        return false;
    }
    if (error < 0) {
        return false;
    }

    git_reference_free(ref);
    return true;
}

std::optional<BranchInfo> Branch::upstream(Repository& repo, std::string_view name) {
    git_reference* ref = nullptr;
    int error = git_branch_lookup(&ref, repo.raw(), std::string(name).c_str(), GIT_BRANCH_LOCAL);
    if (error != 0) {
        return std::nullopt;
    }

    git_reference* upstream_ref = nullptr;
    error = git_branch_upstream(&upstream_ref, ref);
    git_reference_free(ref);

    if (error != 0 || !upstream_ref) {
        return std::nullopt;
    }

    const char* upstream_name = nullptr;
    git_branch_name(&upstream_name, upstream_ref);

    BranchInfo info;
    info.name = upstream_name ? upstream_name : "";
    info.refname = git_reference_name(upstream_ref);
    info.is_remote = true;

    const git_oid* target = git_reference_target(upstream_ref);
    if (target) {
        info.target = Oid(target);
    }

    size_t slash_pos = info.name.find('/');
    if (slash_pos != std::string::npos) {
        info.remote_name = info.name.substr(0, slash_pos);
    }

    git_reference_free(upstream_ref);
    return info;
}

void Branch::set_upstream(Repository& repo, std::string_view branch_name,
                          std::string_view upstream_name) {
    git_reference* ref = nullptr;
    int error = git_branch_lookup(&ref, repo.raw(), std::string(branch_name).c_str(), GIT_BRANCH_LOCAL);
    detail::check_libgit2_error(error, fmt::format("looking up branch {}", branch_name));

    error = git_branch_set_upstream(ref, std::string(upstream_name).c_str());
    git_reference_free(ref);
    detail::check_libgit2_error(error, fmt::format("setting upstream for {}", branch_name));
}

void Branch::unset_upstream(Repository& repo, std::string_view branch_name) {
    git_reference* ref = nullptr;
    int error = git_branch_lookup(&ref, repo.raw(), std::string(branch_name).c_str(), GIT_BRANCH_LOCAL);
    detail::check_libgit2_error(error, fmt::format("looking up branch {}", branch_name));

    error = git_branch_set_upstream(ref, nullptr);
    git_reference_free(ref);
    detail::check_libgit2_error(error, fmt::format("unsetting upstream for {}", branch_name));
}

// =============================================================================
// Tag implementation
// =============================================================================

std::vector<TagInfo> Tag::list(Repository& repo, std::optional<std::string_view> pattern) {
    std::vector<TagInfo> result;

    git_strarray tag_names = {nullptr, 0};
    int error;

    if (pattern) {
        error = git_tag_list_match(&tag_names, std::string(*pattern).c_str(), repo.raw());
    } else {
        error = git_tag_list(&tag_names, repo.raw());
    }
    detail::check_libgit2_error(error, "listing tags");

    for (size_t i = 0; i < tag_names.count; ++i) {
        auto info = get(repo, tag_names.strings[i]);
        if (info) {
            result.push_back(std::move(*info));
        }
    }

    git_strarray_dispose(&tag_names);
    return result;
}

std::vector<std::string> Tag::names(Repository& repo, std::optional<std::string_view> pattern) {
    std::vector<std::string> result;

    git_strarray tag_names = {nullptr, 0};
    int error;

    if (pattern) {
        error = git_tag_list_match(&tag_names, std::string(*pattern).c_str(), repo.raw());
    } else {
        error = git_tag_list(&tag_names, repo.raw());
    }
    detail::check_libgit2_error(error, "listing tag names");

    for (size_t i = 0; i < tag_names.count; ++i) {
        result.emplace_back(tag_names.strings[i]);
    }

    git_strarray_dispose(&tag_names);
    return result;
}

std::optional<TagInfo> Tag::get(Repository& repo, std::string_view name) {
    std::string refname = "refs/tags/" + std::string(name);

    git_reference* ref = nullptr;
    int error = git_reference_lookup(&ref, repo.raw(), refname.c_str());
    if (error == GIT_ENOTFOUND) {
        return std::nullopt;
    }
    detail::check_libgit2_error(error, fmt::format("looking up tag {}", name));

    TagInfo info;
    info.name = std::string(name);
    info.refname = refname;

    const git_oid* target_oid = git_reference_target(ref);
    if (!target_oid) {
        // Symbolic reference, resolve it
        git_reference* resolved = nullptr;
        error = git_reference_resolve(&resolved, ref);
        git_reference_free(ref);
        if (error != 0) {
            return std::nullopt;
        }
        target_oid = git_reference_target(resolved);
        ref = resolved;
    }

    if (target_oid) {
        info.target = Oid(target_oid);
    }

    // Check if this is an annotated tag
    git_object* obj = nullptr;
    error = git_object_lookup(&obj, repo.raw(), target_oid, GIT_OBJECT_ANY);
    if (error == 0 && obj) {
        git_object_t obj_type = git_object_type(obj);

        if (obj_type == GIT_OBJECT_TAG) {
            // Annotated tag
            info.is_annotated = true;

            git_tag* tag = reinterpret_cast<git_tag*>(obj);
            const char* msg = git_tag_message(tag);
            if (msg) {
                info.message = std::string(msg);

                // Check if the message contains a signature
                std::string_view msg_view(msg);
                if (msg_view.find("-----BEGIN PGP SIGNATURE-----") != std::string_view::npos ||
                    msg_view.find("-----BEGIN SSH SIGNATURE-----") != std::string_view::npos) {
                    info.is_signed = true;

                    // Extract the signature from the message
                    // GPG signatures appear at the end of the message
                    size_t sig_start = msg_view.find("-----BEGIN PGP SIGNATURE-----");
                    if (sig_start == std::string_view::npos) {
                        sig_start = msg_view.find("-----BEGIN SSH SIGNATURE-----");
                    }
                    if (sig_start != std::string_view::npos) {
                        info.gpg_signature = std::string(msg_view.substr(sig_start));
                        // Trim the signature from the message
                        info.message = std::string(msg_view.substr(0, sig_start));
                        // Remove trailing whitespace from message
                        while (!info.message->empty() &&
                               (info.message->back() == '\n' || info.message->back() == '\r' ||
                                info.message->back() == ' ')) {
                            info.message->pop_back();
                        }
                    }
                }
            }

            const git_signature* tagger_sig = git_tag_tagger(tag);
            if (tagger_sig) {
                Signature sig;
                sig.name = tagger_sig->name ? tagger_sig->name : "";
                sig.email = tagger_sig->email ? tagger_sig->email : "";
                sig.time = std::chrono::system_clock::from_time_t(tagger_sig->when.time);
                sig.offset_minutes = tagger_sig->when.offset;
                info.tagger = sig;
            }

            // Get the commit target
            git_object* peeled = nullptr;
            if (git_object_peel(&peeled, obj, GIT_OBJECT_COMMIT) == 0) {
                info.commit_target = Oid(git_object_id(peeled));
                git_object_free(peeled);
            } else {
                info.commit_target = info.target;
            }
        } else if (obj_type == GIT_OBJECT_COMMIT) {
            // Lightweight tag
            info.is_annotated = false;
            info.commit_target = info.target;
        }

        git_object_free(obj);
    }

    git_reference_free(ref);
    return info;
}

bool Tag::exists(Repository& repo, std::string_view name) {
    std::string refname = "refs/tags/" + std::string(name);

    git_reference* ref = nullptr;
    int error = git_reference_lookup(&ref, repo.raw(), refname.c_str());

    if (error == GIT_ENOTFOUND) {
        return false;
    }
    if (error < 0) {
        return false;
    }

    git_reference_free(ref);
    return true;
}

void Tag::create_lightweight(Repository& repo, std::string_view name,
                              const Commit& target, bool force) {
    git_oid oid;
    int error = git_tag_create_lightweight(&oid, repo.raw(),
                                           std::string(name).c_str(),
                                           reinterpret_cast<const git_object*>(target.raw()),
                                           force ? 1 : 0);
    detail::check_libgit2_error(error, fmt::format("creating lightweight tag {}", name));
}

void Tag::create_annotated(Repository& repo, std::string_view name,
                           const Commit& target, std::string_view message,
                           std::optional<Signature> tagger, bool force) {
    git_signature* sig = nullptr;

    if (tagger) {
        auto time_t_val = std::chrono::system_clock::to_time_t(tagger->time);
        int error = git_signature_new(&sig, tagger->name.c_str(), tagger->email.c_str(),
                                      time_t_val, tagger->offset_minutes);
        detail::check_libgit2_error(error, "creating tagger signature");
    } else {
        int error = git_signature_default(&sig, repo.raw());
        detail::check_libgit2_error(error, "getting default signature for tag");
    }

    git_oid oid;
    int error = git_tag_create(&oid, repo.raw(),
                               std::string(name).c_str(),
                               reinterpret_cast<const git_object*>(target.raw()),
                               sig,
                               std::string(message).c_str(),
                               force ? 1 : 0);

    git_signature_free(sig);
    detail::check_libgit2_error(error, fmt::format("creating annotated tag {}", name));
}

void Tag::create_signed(Repository& repo, std::string_view name,
                        const Commit& target, std::string_view message,
                        Signer& signer,
                        std::optional<Signature> tagger, bool force) {
    git_signature* sig = nullptr;

    if (tagger) {
        auto time_t_val = std::chrono::system_clock::to_time_t(tagger->time);
        int error = git_signature_new(&sig, tagger->name.c_str(), tagger->email.c_str(),
                                      time_t_val, tagger->offset_minutes);
        detail::check_libgit2_error(error, "creating tagger signature");
    } else {
        int error = git_signature_default(&sig, repo.raw());
        detail::check_libgit2_error(error, "getting default signature for tag");
    }

    // Build the tag content to be signed
    // Format: object <sha>\ntype commit\ntag <name>\ntagger <sig>\n\n<message>
    std::string tag_content;
    tag_content += fmt::format("object {}\n", target.id().to_string());
    tag_content += "type commit\n";
    tag_content += fmt::format("tag {}\n", name);

    // Format tagger line
    char tagger_line[256];
    snprintf(tagger_line, sizeof(tagger_line), "tagger %s <%s> %lld %+03d%02d\n",
             sig->name, sig->email,
             static_cast<long long>(sig->when.time),
             sig->when.offset / 60,
             std::abs(sig->when.offset) % 60);
    tag_content += tagger_line;
    tag_content += "\n";
    tag_content += message;

    // Sign the content
    std::string signature = signer.sign(tag_content);

    // Append signature to message
    std::string signed_message = std::string(message);
    if (!signed_message.empty() && signed_message.back() != '\n') {
        signed_message += '\n';
    }
    signed_message += signature;

    // Create the tag with the signed message
    git_oid oid;
    int error = git_tag_create(&oid, repo.raw(),
                               std::string(name).c_str(),
                               reinterpret_cast<const git_object*>(target.raw()),
                               sig,
                               signed_message.c_str(),
                               force ? 1 : 0);

    git_signature_free(sig);
    detail::check_libgit2_error(error, fmt::format("creating signed tag {}", name));
}

void Tag::remove(Repository& repo, std::string_view name) {
    int error = git_tag_delete(repo.raw(), std::string(name).c_str());
    detail::check_libgit2_error(error, fmt::format("deleting tag {}", name));
}

bool Tag::verify(Repository& repo, std::string_view name, Signer& signer) {
    auto tag_info = get(repo, name);
    if (!tag_info) {
        throw GitError(ErrorCode::NotFound, fmt::format("Tag {} not found", name));
    }

    if (!tag_info->is_signed) {
        throw GitError(ErrorCode::InvalidOperation, fmt::format("Tag {} is not signed", name));
    }

    if (!tag_info->gpg_signature) {
        throw GitError(ErrorCode::InvalidOperation, fmt::format("Tag {} has no signature", name));
    }

    // Get the tag object to extract the signed data
    std::string refname = "refs/tags/" + std::string(name);
    git_reference* ref = nullptr;
    int error = git_reference_lookup(&ref, repo.raw(), refname.c_str());
    detail::check_libgit2_error(error, fmt::format("looking up tag {}", name));

    const git_oid* target_oid = git_reference_target(ref);
    git_reference_free(ref);

    git_tag* tag = nullptr;
    error = git_tag_lookup(&tag, repo.raw(), target_oid);
    if (error != 0) {
        // Might be a lightweight tag
        return false;
    }

    // The signed data is the tag content without the signature
    // We need to reconstruct it
    const git_signature* tagger_sig = git_tag_tagger(tag);

    git_object* tagged_obj = nullptr;
    git_tag_target(&tagged_obj, tag);
    const git_oid* tagged_oid = git_object_id(tagged_obj);

    std::string signed_data;
    signed_data += fmt::format("object {}\n", Oid(tagged_oid).to_string());
    signed_data += fmt::format("type {}\n", git_object_type2string(git_object_type(tagged_obj)));
    signed_data += fmt::format("tag {}\n", git_tag_name(tag));

    if (tagger_sig) {
        char tagger_line[256];
        snprintf(tagger_line, sizeof(tagger_line), "tagger %s <%s> %lld %+03d%02d\n",
                 tagger_sig->name, tagger_sig->email,
                 static_cast<long long>(tagger_sig->when.time),
                 tagger_sig->when.offset / 60,
                 std::abs(tagger_sig->when.offset) % 60);
        signed_data += tagger_line;
    }
    signed_data += "\n";

    // Add the message without the signature
    if (tag_info->message) {
        signed_data += *tag_info->message;
    }

    git_object_free(tagged_obj);
    git_tag_free(tag);

    // Verify the signature
    auto result = signer.verify(signed_data, *tag_info->gpg_signature);
    return result.valid;
}

std::vector<std::string> Tag::pointing_to(Repository& repo, const Oid& commit_oid) {
    std::vector<std::string> result;

    auto all_tags = list(repo);
    for (const auto& tag : all_tags) {
        if (tag.commit_target == commit_oid) {
            result.push_back(tag.name);
        }
    }

    return result;
}

}  // namespace gitmanip
