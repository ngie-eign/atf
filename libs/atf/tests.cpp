//
// Automated Testing Framework (atf)
//
// Copyright (c) 2007 The NetBSD Foundation, Inc.
// All rights reserved.
//
// This code is derived from software contributed to The NetBSD Foundation
// by Julio M. Merino Vidal, developed as part of Google's Summer of Code
// 2007 program.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
// 1. Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
// 3. All advertising materials mentioning features or use of this
//    software must display the following acknowledgement:
//        This product includes software developed by the NetBSD
//        Foundation, Inc. and its contributors.
// 4. Neither the name of The NetBSD Foundation nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND
// CONTRIBUTORS ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES,
// INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
// IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
// GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
// IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
// OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
// IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//

extern "C" {
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
}

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "atf/application.hpp"
#include "atf/config.hpp"
#include "atf/env.hpp"
#include "atf/exceptions.hpp"
#include "atf/expand.hpp"
#include "atf/formats.hpp"
#include "atf/fs.hpp"
#include "atf/io.hpp"
#include "atf/serial.hpp"
#include "atf/tests.hpp"
#include "atf/text.hpp"
#include "atf/ui.hpp"
#include "atf/user.hpp"

namespace impl = atf::tests;
#define IMPL_NAME "atf::tests"

// ------------------------------------------------------------------------
// The "tcr" class.
// ------------------------------------------------------------------------

impl::tcr::tcr(void) :
    m_status(status_failed),
    m_reason("Uninitialized test case result")
{
}

impl::tcr::tcr(impl::tcr::status s, const std::string& r) :
    m_status(s),
    m_reason(r)
{
    assert(m_reason.find('\n') == std::string::npos);
}

impl::tcr
impl::tcr::passed(void)
{
    return tcr(status_passed, "");
}

impl::tcr
impl::tcr::skipped(const std::string& reason)
{
    return tcr(status_skipped, reason);
}

impl::tcr
impl::tcr::failed(const std::string& reason)
{
    return tcr(status_failed, reason);
}

impl::tcr::status
impl::tcr::get_status(void)
    const
{
    return m_status;
}

const std::string&
impl::tcr::get_reason(void)
    const
{
    return m_reason;
}

// ------------------------------------------------------------------------
// The "tc" class.
// ------------------------------------------------------------------------

impl::tc::tc(const std::string& ident) :
    m_ident(ident)
{
}

impl::tc::~tc(void)
{
}

void
impl::tc::ensure_boolean(const std::string& name)
{
    ensure_not_empty(name);

    std::string val = text::to_lower(get(name));
    if (val == "yes" || val == "true")
        set(name, "true");
    else if (val == "no" || val == "false")
        set(name, "false");
    else
        throw std::runtime_error("Invalid value for boolean variable `" +
                                 name + "'");
}

void
impl::tc::ensure_not_empty(const std::string& name)
{
    if (m_meta_data.find(name) == m_meta_data.end())
        throw atf::not_found_error< std::string >
            ("Undefined or empty variable", name);

    variables_map::const_iterator iter = m_meta_data.find(name);
    assert(iter != m_meta_data.end());

    const std::string& val = (*iter).second;
    if (val.empty())
        throw atf::not_found_error< std::string > // XXX Incorrect error
            ("Undefined or empty variable", name);
}

void
impl::tc::set(const std::string& var, const std::string& val)
{
    m_meta_data[var] = val;
}

const std::string&
impl::tc::get(const std::string& var)
    const
{
    variables_map::const_iterator iter = m_meta_data.find(var);
    assert(iter != m_meta_data.end());
    return (*iter).second;
}

bool
impl::tc::get_bool(const std::string& var)
    const
{
    std::string val = get(var);

    if (val == "true")
        return true;
    else if (val == "false")
        return false;
    else {
        assert(false);
        return false;
    }
}

bool
impl::tc::has(const std::string& var)
    const
{
    variables_map::const_iterator iter = m_meta_data.find(var);
    return (iter != m_meta_data.end());
}

const std::string&
impl::tc::get_srcdir(void)
    const
{
    return m_srcdir;
}

void
impl::tc::init(const std::string& srcdir, const std::string& workdirbase)
{
    assert(m_meta_data.empty());

    m_srcdir = srcdir;
    m_workdirbase = workdirbase;

    m_meta_data["ident"] = m_ident;
#include "tchead.cpp"
    assert(m_meta_data["ident"] == m_ident);
}

static
void
enter_workdir(const impl::tc* tc, atf::fs::path& olddir,
              atf::fs::path& workdir, const std::string& base)
{
    if (!tc->get_bool("isolated"))
        return;

    atf::fs::path pattern = atf::fs::path(base) / "atf.XXXXXX";
    workdir = atf::fs::create_temp_dir(pattern);
    olddir = atf::fs::change_directory(workdir);
}

static
void
leave_workdir(const impl::tc* tc, atf::fs::path& olddir,
              atf::fs::path& workdir)
{
    if (!tc->get_bool("isolated"))
        return;

    atf::fs::change_directory(olddir);
    atf::fs::cleanup(workdir);
}

impl::tcr
impl::tc::safe_run(void)
    const
{
    tcr tcr = tcr::passed();

    if (has("require.user")) {
        const std::string& u = get("require.user");
        if (u == "root") {
            if (!user::is_root())
                tcr = tcr::skipped("Requires root privileges");
        } else if (u == "unprivileged") {
            if (!user::is_unprivileged())
                tcr = tcr::skipped("Requires an unprivileged "
                                                "user");
        } else
            tcr = tcr::skipped("Invalid value in the "
                                            "require.user property");
    }

    if (tcr.get_status() == tcr::status_passed) {
        // Previous checks have not made the test fail, so run it now.

        fs::path olddir(".");
        fs::path workdir(".");

        try {
            enter_workdir(this, olddir, workdir, m_workdirbase);
            tcr = fork_body(workdir.str());
            leave_workdir(this, olddir, workdir);
        } catch (...) {
            leave_workdir(this, olddir, workdir);
            throw;
        }
    }

    return tcr;
}

impl::tcr
impl::tc::fork_body(const std::string& workdir)
    const
{
    tcr tcr;

    fs::path result(".");
    if (get_bool("isolated")) {
        result = fs::path(workdir) / "tc-result";
    } else {
        result = fs::create_temp_file(fs::path(m_workdirbase) / "atf.XXXXXX");
    }

    pid_t pid = ::fork();
    if (pid == -1) {
        tcr = tcr::failed("Coult not fork to run test case");
    } else if (pid == 0) {
        // Unexpected errors detected in the child process are mentioned
        // in stderr to give the user a chance to see what happened.
        // This is specially useful in those cases where these errors
        // cannot be directed to the parent process.
        try {
#include "tcenv.cpp"

            // This is here because require_progs can assert if the user
            // gave bad input.  Having it here lets us capture the abort
            // from the parent.
            //
            // XXX The thing is... is this appropriate?  Should we forbid
            // the assertions in our code and use exceptions instead?
            // Or... should all properties be handled here -- aka, in the
            // controlled environment?
            if (has("require.progs")) {
                std::vector< std::string > progs =
                    text::split(get("require.progs"), " ");
                for (std::vector< std::string >::const_iterator iter =
                     progs.begin(); iter != progs.end(); iter++)
                    require_prog(*iter);
            }

            body();
            throw impl::tcr::passed();
        } catch (const impl::tcr& tcre) {
            std::ofstream os(result.c_str());
            if (!os) {
                std::cerr << "Could not open the temporary file " +
                             result.str() + " to leave the results in it"
                          << std::endl;
                std::exit(EXIT_FAILURE);
            }
            if (tcre.get_status() == impl::tcr::status_passed)
                os << "passed\n";
            else if (tcre.get_status() == impl::tcr::status_failed)
                os << "failed\n" << tcre.get_reason() << '\n';
            else if (tcre.get_status() == impl::tcr::status_skipped)
                os << "skipped\n" << tcre.get_reason() << '\n';
            os.close();
        } catch (const std::runtime_error& e) {
            std::cerr << "Caught unexpected error: " << e.what() << std::endl;
            std::exit(EXIT_FAILURE);
        } catch (...) {
            std::cerr << "Caught unexpected error" << std::endl;
            std::exit(EXIT_FAILURE);
        }
        std::exit(EXIT_SUCCESS);
    } else {
        int status;
        if (::waitpid(pid, &status, 0) == -1)
            tcr = tcr::failed("waitpid failed");
        else {
            std::ifstream is(result.c_str());
            if (!is) {
                tcr = tcr::failed("Could not open results file "
                                               "for reading");
            } else {
                std::string line;
                std::getline(is, line);
                if (line == "passed") {
                    tcr = tcr::passed();
                } else if (line == "failed") {
                    std::getline(is, line);
                    tcr = tcr::failed(line);
                } else if (line == "skipped") {
                    std::getline(is, line);
                    tcr = tcr::skipped(line);
                } else {
                    tcr = tcr::failed("Test case failed to "
                                                   "report its status");
                }
                is.close();
            }
        }
    }

    return tcr;
}

impl::tcr
impl::tc::run(void)
    const
{
    assert(!m_meta_data.empty());

    tcr tcr;

    try {
        tcr = safe_run();
    } catch (const std::exception& e) {
        tcr = tcr::failed(std::string("Unhandled exception: ") +
                          e.what());
    } catch (...) {
        tcr = tcr::failed("Unknown unhandled exception");
    }

    return tcr;
}

void
impl::tc::require_prog(const std::string& prog)
    const
{
    assert(!prog.empty());

    fs::path p(prog);

    if (p.is_absolute()) {
        if (!fs::is_executable(p))
            throw impl::tcr::skipped("The required program " + prog +
                                     " could not be found");
    } else {
        assert(p.branch_path() == fs::path("."));
        if (fs::find_prog_in_path(prog) == fs::path("."))
            throw impl::tcr::skipped("The required program " + prog +
                                     " could not be found in the PATH");
    }
}

// ------------------------------------------------------------------------
// The "tp" class.
// ------------------------------------------------------------------------

class tp : public atf::application {
public:
    typedef std::vector< impl::tc * > tc_vector;

private:
    static const char* m_description;

    bool m_lflag;
    int m_results_fd;
    std::auto_ptr< std::ostream > m_results_os;
    atf::fs::path m_srcdir;
    atf::fs::path m_workdir;
    std::set< std::string > m_tcnames;

    std::string specific_args(void) const;
    options_set specific_options(void) const;
    void process_option(int, const char*);

    tc_vector m_tcs;

    tc_vector init_tcs(void);
    static tc_vector filter_tcs(tc_vector, const std::set< std::string >&);

    std::ostream& results_stream(void);

    int list_tcs(void);
    int run_tcs(void);

public:
    tp(const tc_vector&);

    int main(void);
};

const char* tp::m_description =
    "This is an independent atf test program.";

tp::tp(const tc_vector& tcs) :
    application(m_description),
    m_lflag(false),
    m_results_fd(STDOUT_FILENO),
    m_srcdir(atf::fs::get_current_dir()),
    m_workdir(atf::config::get("atf_workdir")),
    m_tcs(tcs)
{
}

std::string
tp::specific_args(void)
    const
{
    return "[test_case1 [.. test_caseN]]";
}

tp::options_set
tp::specific_options(void)
    const
{
    options_set opts;
    opts.insert(option('l', "", "List test cases and their purpose"));
    opts.insert(option('r', "fd", "The file descriptor to which the test "
                                  "program will send the results of the "
                                  "test cases"));
    opts.insert(option('s', "srcdir", "Directory where the test's data "
                                      "files are located"));
    opts.insert(option('w', "workdir", "Base directory where the test cases "
                                       "will put temporary files"));
    return opts;
}

void
tp::process_option(int ch, const char* arg)
{
    switch (ch) {
    case 'l':
        m_lflag = true;
        break;

    case 'r':
        {
            std::istringstream ss(arg);
            ss >> m_results_fd;
        }
        break;

    case 's':
        m_srcdir = atf::fs::path(arg);
        break;

    case 'w':
        m_workdir = atf::fs::path(arg);
        break;

    default:
        assert(false);
    }
}

tp::tc_vector
tp::init_tcs(void)
{
    tc_vector tcs = m_tcs;

    for (tc_vector::iterator iter = tcs.begin();
         iter != tcs.end(); iter++) {
        impl::tc* tc = *iter;

        tc->init(m_srcdir.str(), m_workdir.str());
    }

    return tcs;
}

//
// An auxiliary unary predicate that compares the given test case's
// identifier to the identifier stored in it.
//
class tc_equal_to_ident {
    const std::string& m_ident;

public:
    tc_equal_to_ident(const std::string& i) :
        m_ident(i)
    {
    }

    bool operator()(const impl::tc* tc)
    {
        return tc->get("ident") == m_ident;
    }
};

tp::tc_vector
tp::filter_tcs(tc_vector tcs, const std::set< std::string >& tcnames)
{
    tc_vector tcso;

    if (tcnames.empty()) {
        // Special case: added for efficiency because this is the most
        // typical situation.
        tcso = tcs;
    } else {
        // Collect all the test cases' identifiers.
        std::set< std::string > ids;
        for (tc_vector::iterator iter = tcs.begin();
             iter != tcs.end(); iter++) {
            impl::tc* tc = *iter;

            ids.insert(tc->get("ident"));
        }

        // Iterate over all names provided by the user and, for each one,
        // expand it as if it were a glob pattern.  Collect all expansions.
        std::set< std::string > exps;
        for (std::set< std::string >::const_iterator iter = tcnames.begin();
             iter != tcnames.end(); iter++) {
            const std::string& glob = *iter;

            std::set< std::string > ms = atf::expand::expand_glob(glob, ids);
            if (ms.empty())
                throw std::runtime_error("Unknown test case `" + glob + "'");
            exps.insert(ms.begin(), ms.end());
        }

        // For each expansion, locate its corresponding test case and add
        // it to the output set.
        for (std::set< std::string >::const_iterator iter = exps.begin();
             iter != exps.end(); iter++) {
            const std::string& name = *iter;

            tc_vector::iterator tciter =
                std::find_if(tcs.begin(), tcs.end(), tc_equal_to_ident(name));
            assert(tciter != tcs.end());
            tcso.push_back(*tciter);
        }
    }

    return tcso;
}

int
tp::list_tcs(void)
{
    tc_vector tcs = filter_tcs(init_tcs(), m_tcnames);

    std::string::size_type maxlen = 0;
    for (tc_vector::const_iterator iter = tcs.begin();
         iter != tcs.end(); iter++) {
        const impl::tc* tc = *iter;

        if (maxlen < tc->get("ident").length())
            maxlen = tc->get("ident").length();
    }

    for (tc_vector::const_iterator iter = tcs.begin();
         iter != tcs.end(); iter++) {
        const impl::tc* tc = *iter;

        std::cout << atf::ui::format_text_with_tag(tc->get("descr"),
                                                   tc->get("ident"),
                                                   false, maxlen + 4)
                  << std::endl;
    }

    return EXIT_SUCCESS;
}

std::ostream&
tp::results_stream(void)
{
    if (m_results_fd == STDOUT_FILENO)
        return std::cout;
    else if (m_results_fd == STDERR_FILENO)
        return std::cerr;
    else
        return *m_results_os;
}

int
tp::run_tcs(void)
{
    tc_vector tcs = filter_tcs(init_tcs(), m_tcnames);

    int errcode = EXIT_SUCCESS;

    atf::formats::atf_tcs_writer w(results_stream(), tcs.size());
    for (tc_vector::iterator iter = tcs.begin();
         iter != tcs.end(); iter++) {
        impl::tc* tc = *iter;

        w.start_tc(tc->get("ident"));
        impl::tcr tcr = tc->run();
        w.end_tc(tcr);

        if (tcr.get_status() == impl::tcr::status_failed)
            errcode = EXIT_FAILURE;
    }

    return errcode;
}

int
tp::main(void)
{
    int errcode;

    if (!atf::fs::exists(m_srcdir / m_prog_name))
        throw std::runtime_error("Cannot find the test program in the "
                                 "source directory `" + m_srcdir.str() + "'");

    if (!atf::fs::exists(m_workdir))
        throw std::runtime_error("Cannot find the work directory `" +
                                 m_workdir.str() + "'");

    for (int i = 0; i < m_argc; i++)
        m_tcnames.insert(m_argv[i]);

    if (m_lflag)
        errcode = list_tcs();
    else {
        if (m_results_fd != STDOUT_FILENO && m_results_fd != STDERR_FILENO) {
            atf::io::file_handle fh(m_results_fd);
            m_results_os =
                std::auto_ptr< std::ostream >(new atf::io::postream(fh));
        }
        errcode = run_tcs();
    }

    return errcode;
}

namespace atf {
    namespace tests {
        int run_tp(int, char* const*, const tp::tc_vector&);
    }
}

int
impl::run_tp(int argc, char* const* argv, const tp::tc_vector& tcs)
{
    return tp(tcs).run(argc, argv);
}