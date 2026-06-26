#ifndef CLLAMA_UTIL_CFLAG_HPP
#define CLLAMA_UTIL_CFLAG_HPP

#include <string>
#include <vector>
#include <list>
#include <memory>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <functional>
#include <algorithm>
#include <cctype>

namespace cllama {
namespace util {

// ── helpers ──────────────────────────────────────────────────────

inline static std::string trim(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

// ── Value (type-erased storage) ──────────────────────────────────

struct IValue {
    virtual ~IValue() = default;
    virtual void set(const std::string& raw) = 0;
    virtual std::string repr() const = 0;
};

template <typename T>
struct TypedValue final : IValue {
    T* target;
    T  fallback;
    bool* was_set = nullptr;
    TypedValue(T* t, T f) : target(t), fallback(f) { if (target) *target = fallback; }
    void set(const std::string& raw) override {
        if (!target) return;
        if constexpr (std::is_same_v<T, std::string>)  *target = raw;
        else if constexpr (std::is_same_v<T, int>)      *target = std::stoi(raw);
        else if constexpr (std::is_same_v<T, float>)    *target = std::stof(raw);
        else if constexpr (std::is_same_v<T, bool>)     *target = true;
        if (was_set) *was_set = true;
    }
    std::string repr() const override {
        if constexpr (std::is_same_v<T, std::string>)  return fallback;
        else if constexpr (std::is_same_v<T, bool>)    return fallback ? "true" : "false";
        else if constexpr (std::is_same_v<T, float>) {
            std::ostringstream ss; ss << fallback; return ss.str();
        } else                                           return std::to_string(fallback);
    }
};

// ── Option (--name value / --name=value / -n value) ──────────────

class Option {
public:
    std::string lname;
    char        sname = 0;
    std::string desc;
    bool        is_flag = false;
    bool        required = false;
    bool        was_set = false;
    std::shared_ptr<IValue> val;

    std::string names() const {
        std::string r;
        if (sname)  r += std::string("-") + sname + ", ";
        r += lname;
        return r;
    }

    bool match(const std::string& arg) const {
        return arg == lname || (sname && arg == std::string("-") + sname);
    }
};

// ── Argument (positional) ────────────────────────────────────────

class Argument {
public:
    std::string name;
    std::string desc;
    bool        required = true;
    std::shared_ptr<IValue> val;
};

// ── Command (subcommand) ─────────────────────────────────────────

class Command {
public:
    std::string name;
    std::string desc;
    bool        is_default = false;

    template <typename T>
    Command& add_option(const std::string& full, T* target, T fallback, const std::string& description = "", bool required = false) {
        Option opt;
        auto pos = full.find(',');
        if (pos != std::string::npos) {
            auto first = trim(full.substr(0, pos));
            auto second = trim(full.substr(pos + 1));
            if (first.size() == 2 && first[0] == '-')      opt.sname = first[1];
            else if (second.size() == 2 && second[0] == '-') opt.sname = second[1];
            opt.lname = (first[0] == '-' ? first : second);
        } else {
            opt.lname = full;
        }
        opt.desc     = description;
        opt.required = required;
        auto tv = std::make_shared<TypedValue<T>>(target, fallback);
        tv->was_set = &opt.was_set;
        opt.val = tv;
        options_.push_back(std::move(opt));
        tv->was_set = &options_.back().was_set;
        return *this;
    }

    Command& add_option(const std::string& full, std::string* target, const char* fallback, const std::string& description = "", bool required = false) {
        return add_option<std::string>(full, target, std::string(fallback), description, required);
    }

    Command& add_flag(const std::string& full, bool* target, const std::string& description = "", bool required = false) {
        Option opt;
        auto pos = full.find(',');
        if (pos != std::string::npos) {
            auto first = trim(full.substr(0, pos));
            auto second = trim(full.substr(pos + 1));
            if (first.size() == 2 && first[0] == '-')      opt.sname = first[1];
            else if (second.size() == 2 && second[0] == '-') opt.sname = second[1];
            opt.lname = (first[0] == '-' ? first : second);
        } else {
            opt.lname = full;
        }
        opt.desc     = description;
        opt.is_flag  = true;
        opt.required = required;
        auto tv = std::make_shared<TypedValue<bool>>(target, false);
        tv->was_set = &opt.was_set;
        opt.val     = tv;
        options_.push_back(std::move(opt));
        tv->was_set = &options_.back().was_set;
        return *this;
    }

    template <typename T>
    Command& add_argument(const std::string& name, T* target, const std::string& description = "", bool required = true) {
        Argument arg;
        arg.name     = name;
        arg.desc     = description;
        arg.required = required;
        arg.val      = std::make_shared<TypedValue<T>>(target, T{});
        arguments_.push_back(std::move(arg));
        return *this;
    }

    template <typename T>
    Command& add_optional_argument(const std::string& name, T* target, T fallback, const std::string& description = "") {
        Argument arg;
        arg.name     = name;
        arg.desc     = description;
        arg.required = false;
        arg.val      = std::make_shared<TypedValue<T>>(target, fallback);
        arguments_.push_back(std::move(arg));
        return *this;
    }

    // ── subcommands ─────────────────────────────────────────────

    Command& add_subcommand(const std::string& name, const std::string& desc) {
        auto cmd = std::make_shared<Command>();
        cmd->name = name;
        cmd->desc = desc;
        cmd->parent = this;
        subcommands_.push_back(cmd);
        return *cmd;
    }

    Command* find_subcommand(const std::string& name) const {
        for (const auto& s : subcommands_)
            if (s->name == name) return s.get();
        return nullptr;
    }

    bool has_subcommands() const { return !subcommands_.empty(); }

    // ── internal ──────────────────────────────────────────────

    const std::list<Option>&     options()   const { return options_; }
    const std::vector<Argument>& arguments() const { return arguments_; }
    const std::vector<std::shared_ptr<Command>>& subcommands() const { return subcommands_; }

    Option* find_option(const std::string& arg) {
        for (auto& o : options_) if (o.match(arg)) return &o;
        return nullptr;
    }

    void print_help(const std::string& prog) const {
        std::cerr << "Usage: " << prog;
        if (!name.empty()) std::cerr << " " << full_name();
        if (has_subcommands()) {
            std::cerr << " <command>";
        } else {
            for (const auto& a : arguments_)
                std::cerr << (a.required ? " <" : " [") << a.name << (a.required ? ">" : "]");
            if (!options_.empty()) std::cerr << " [options]";
        }
        std::cerr << "\n\n" << desc << "\n\n";

        if (!arguments_.empty()) {
            size_t mw = 0;
            for (const auto& a : arguments_) mw = std::max(mw, a.name.size());
            std::cerr << "Arguments:\n";
            for (const auto& a : arguments_) {
                std::cerr << "  " << a.name << std::string(mw - a.name.size() + 2, ' ') << a.desc << "\n";
            }
            std::cerr << "\n";
        }

        if (has_subcommands()) {
            std::cerr << "Commands:\n";
            for (const auto& s : subcommands_) {
                std::cerr << "  " << s->name
                          << std::string(size_t(18) > s->name.size() ? 18 - s->name.size() : 2, ' ')
                          << s->desc << "\n";
            }
            std::cerr << "\n";
        }

        std::cerr << "Options:\n";
        for (const auto& o : options_) {
            std::string n = o.names();
            std::cerr << "  " << n << std::string(size_t(24) > n.size() ? 24 - n.size() : 2, ' ') << o.desc;
            if (o.required)
                std::cerr << " (required)";
            else if (o.val && !o.is_flag && !o.val->repr().empty())
                std::cerr << " (default: " << o.val->repr() << ")";
            std::cerr << "\n";
        }
    }

public:
    Command* parent = nullptr;

    std::string full_name() const {
        if (parent && !parent->name.empty())
            return parent->full_name() + " " + name;
        return name;
    }

private:
    std::list<Option>     options_;
    std::vector<Argument> arguments_;
    std::vector<std::shared_ptr<Command>> subcommands_;

    friend class App;
};

// ── App (top-level parser) ───────────────────────────────────────

class App {
public:
    App(const std::string& prog, const std::string& desc)
        : prog_(prog), desc_(desc) {}

    Command& add_command(const std::string& name, const std::string& desc) {
        auto cmd = std::make_shared<Command>();
        cmd->name = name;
        cmd->desc = desc;
        commands_.push_back(cmd);
        return *cmd;
    }

    void set_default_command(Command& cmd) {
        default_ = &cmd;
        default_->is_default = true;
    }

    Command* parse(int argc, char* argv[]) {
        if (argc < 2) {
            if (default_) return default_;
            print_help();
            std::exit(0);
        }

        // collect argv into strings
        std::vector<std::string> args;
        for (int i = 1; i < argc; ++i) args.push_back(argv[i]);

        // "help [cmd...]" → show help
        if (args.size() >= 1 && args[0] == "help") {
            if (args.size() >= 2) {
                auto* cmd = find_deep(args, 1);
                if (cmd) { cmd->print_help(prog_); std::exit(0); }
                std::cerr << "Unknown command: " << args[1];
                for (size_t hi = 2; hi < args.size(); ++hi) std::cerr << " " << args[hi];
                std::cerr << "\n";
            }
            print_help();
            std::exit(0);
        }

        // "--help" / "-h" with no command → global help
        if (args[0] == "--help" || args[0] == "-h") {
            print_help();
            std::exit(0);
        }

        // walk through command hierarchy
        std::vector<std::string> consumed;
        Command* cmd = nullptr;
        size_t idx = 0;

        // try matching top-level commands
        for (const auto& c : commands_) {
            if (c->name == args[0]) {
                cmd = c.get();
                idx = 1;
                consumed.push_back(c->name);
                break;
            }
        }

        // if no command matched, use default
        if (!cmd) {
            if (default_) {
                cmd = default_;
                idx = 0;
            } else {
                std::cerr << "Unknown command: " << args[0] << "\n";
                print_help();
                std::exit(1);
            }
        }

        // descend into subcommands
        while (idx < args.size()) {
            auto* sub = cmd->find_subcommand(args[idx]);
            if (!sub) break;
            cmd = sub;
            consumed.push_back(args[idx]);
            ++idx;
        }

        // "--help" / "-h" on any command
        if (idx < args.size() && (args[idx] == "--help" || args[idx] == "-h")) {
            cmd->print_help(prog_);
            std::exit(0);
        }

        // parse options & positional args
        std::vector<std::string> positional;
        for (; idx < args.size(); ++idx) {
            const std::string& a = args[idx];

            // --name=value
            if (a.size() > 2 && a[0] == '-' && a[1] == '-') {
                auto eq = a.find('=');
                std::string name = (eq != std::string::npos) ? a.substr(0, eq) : a;
                Option* opt = cmd->find_option(name);
                if (!opt) {
                    std::cerr << "Unknown option: " << a << "\n";
                    cmd->print_help(prog_);
                    std::exit(1);
                }
                if (opt->is_flag) {
                    opt->val->set("true");
                } else if (eq != std::string::npos) {
                    opt->val->set(a.substr(eq + 1));
                } else if (++idx < args.size()) {
                    opt->val->set(args[idx]);
                } else {
                    std::cerr << "Option " << name << " requires a value\n";
                    cmd->print_help(prog_);
                    std::exit(1);
                }
                continue;
            }

            // -s value
            if (a.size() == 2 && a[0] == '-') {
                bool found = false;
                for (auto& opt : cmd->options()) {
                    Option* o = const_cast<Option*>(&opt);
                    if (o->sname == a[1]) {
                        if (o->is_flag) {
                            o->val->set("true");
                        } else if (++idx < args.size()) {
                            o->val->set(args[idx]);
                        } else {
                            std::cerr << "Option -" << a[1] << " requires a value\n";
                            cmd->print_help(prog_);
                            std::exit(1);
                        }
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    std::cerr << "Unknown option: " << a << "\n";
                    cmd->print_help(prog_);
                    std::exit(1);
                }
                continue;
            }

            // positional argument
            positional.push_back(a);
        }

        // bind positional args
        const auto& cmd_args = cmd->arguments();
        for (size_t i = 0; i < positional.size(); ++i) {
            if (i < cmd_args.size()) {
                if (cmd_args[i].val)
                    cmd_args[i].val->set(positional[i]);
            }
        }

        // check required positional arguments
        for (size_t i = positional.size(); i < cmd_args.size(); ++i) {
            if (cmd_args[i].required) {
                std::cerr << "Missing required argument: " << cmd_args[i].name << "\n";
                cmd->print_help(prog_);
                std::exit(1);
            }
        }

        // check required options
        for (const auto& opt : cmd->options()) {
            if (opt.required && !opt.was_set) {
                std::cerr << "Missing required option: " << opt.names() << "\n";
                cmd->print_help(prog_);
                std::exit(1);
            }
        }

        return cmd;
    }

    void print_help() const {
        std::cerr << "Usage: " << prog_ << " <command> [options]\n\n"
                  << desc_ << "\n\nCommands:\n";
        for (const auto& cmd : commands_) {
            if (cmd->is_default) continue;
            if (cmd->has_subcommands())
                print_subcommands(*cmd, "  " + cmd->name + " ");
            else
                std::cerr << "  " << cmd->name
                          << std::string(size_t(18) > cmd->name.size() ? 18 - cmd->name.size() : 2, ' ')
                          << cmd->desc << "\n";
        }
        if (default_) {
            std::cerr << "\nUse '" << prog_ << " <command> --help' for command-specific help.\n";
        }
    }

private:
    Command* find_deep(const std::vector<std::string>& names, size_t start) const {
        for (const auto& c : commands_) {
            if (c->name == names[start]) {
                Command* cmd = c.get();
                for (size_t i = start + 1; i < names.size(); ++i) {
                    auto* sub = cmd->find_subcommand(names[i]);
                    if (!sub) return cmd;
                    cmd = sub;
                }
                return cmd;
            }
        }
        return nullptr;
    }

    void print_subcommands(const Command& cmd, const std::string& prefix) const {
        for (const auto& s : cmd.subcommands()) {
            std::string full = prefix + s->name;
            if (s->has_subcommands())
                print_subcommands(*s, prefix + s->name + " ");
            else
                std::cerr << full
                          << std::string(size_t(24) > full.size() ? 24 - full.size() : 2, ' ')
                          << s->desc << "\n";
        }
    }

    std::string prog_;
    std::string desc_;
    std::vector<std::shared_ptr<Command>> commands_;
    Command* default_ = nullptr;
};

} // namespace util
} // namespace cllama

#endif // CLLAMA_UTIL_CFLAG_HPP
