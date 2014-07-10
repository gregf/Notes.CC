#include <iostream>
#include <algorithm>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string>
#include <cstring>
#include <list>

#include <Project.h>
#include <Note.h>
#include <Colors.h>
#include <TableView.h>
#include <Filter.h>

#include <git2.h>

// Readline
#include <readline/readline.h>
#include <readline/history.h>

#include <settings.h>

struct timespec _tick_start;
struct timespec _tick_stop;

#define INIT_TIC_TAC()    { clock_gettime ( CLOCK_REALTIME, &_tick_start ); }

#define TIC( a )          {                                                              \
        clock_gettime ( CLOCK_REALTIME, &_tick_stop );                                   \
        timespec diff;                                                                   \
        diff.tv_sec  = _tick_stop.tv_sec - _tick_start.tv_sec;                           \
        diff.tv_nsec = _tick_stop.tv_nsec - _tick_start.tv_nsec;                         \
        if ( diff.tv_nsec < 0 ) {                                                        \
            diff.tv_sec  -= 1;                                                           \
            diff.tv_nsec += 1e9;                                                         \
        }                                                                                \
        notes_print_info ( "%s: %lu s, %.2f ms\n", a, diff.tv_sec, diff.tv_nsec / 1e6 ); \
}





// List of supported commands.
const char * commands[] =
{
    "add",
    "move",
    "edit",
    "view",
    "cat",
    "list",
    "push",
    "pull",
    "export",
    "delete",
    "projects",
    "interactive",
    nullptr
};

/**
 * This project is written in C++, but tries to stick closer to C.
 * Classes, list, strings are ok.. Templates only when it safes duplication.
 */



// The Main object, this is also the root node.
class NotesCC : public Project
{
private:
    unsigned int        last_note_id = 0;
// Root project.
    std::vector<Note *> notes;

    git_repository      *git_repo        = nullptr;
    git_index           * git_repo_index = nullptr;
    bool                git_changed      = false;
    Settings            settings;

public:
    NotesCC( )  : Project ( "" )
    {
    }
    ~NotesCC()
    {
        if ( git_changed ) {
            notes_print_info ( "Commiting changes to git.\n" );
            repository_commit_changes ();
            git_changed = false;
        }

        // Clean git references.
        if ( git_repo_index != nullptr ) {
            git_index_free ( git_repo_index );
            git_repo_index = nullptr;
        }

        if ( git_repo != nullptr ) {
            git_repository_free ( git_repo );
            git_repo = nullptr;
        }
        // Clear internal state.
        clear ();
    }
    void run ( int argc, char **argv )
    {
        // Check interactive mode.
        if ( argc == 2 && strcmp ( argv[1], "interactive" ) == 0 ) {
            notes_print_info ( "Interactive mode\n" );
            interactive ();
        }

        // Check autocomplete.
        else if ( argc > 1 && strcmp ( argv[1], "--complete" ) == 0 ) {
            notes_print_quiet ();
            run_autocomplete ( argc - 1, &argv[1] );
        }

        // Commandline parser
        else {
            cmd_parser ( argc - 1, &argv[1] );
        }
    }

    /**
     * Open the note repository.
     */
    bool open_repository ( )
    {
        const char *db_path = settings.get_repository ().c_str ();
        // Check git repository.
        if ( git_repository_open ( &git_repo, db_path ) != 0 ) {
            notes_print_error ( "The repository directory '%s' is not a git repository.\n",
                                db_path );
            notes_print_error ( "Please initialize a new repository using git init.\n" );
            return false;
        }
        if ( !this->check_repository_state () ) {
            return false;
        }

        // Load the notes.
        this->Load ( );

        return true;
    }

private:
    /**
     * This function is used to sort the notes and assign them an id.
     * Sorting should be stable between runs (when there is no edit of note).
     */
    static bool notes_print_sort ( Note *a, Note *b )
    {
        time_t diff_time = ( a->get_time_t () - b->get_time_t () );
        // If they are of equal time.
        if ( diff_time == 0 ) {
            int retv = a->get_title ().compare ( b->get_title () );
            if ( retv < 0 ) {
                return true;
            }
            // Sort on hash as a last resort, so we get a stable sort.
            return a->get_body_crc () - b->get_body_crc () > 0;
        }
        // If A is later then b it should go above a.
        return diff_time < 0;
    }

    /**
     * Repository interaction functions.
     */
    git_commit * repository_get_last_commit ( )
    {
        int        rc;
        git_commit * commit = NULL;   /* the result */
        git_oid    oid_parent_commit; /* the SHA1 for last commit */

        /* resolve HEAD into a SHA1 */
        rc = git_reference_name_to_id ( &oid_parent_commit, git_repo, "HEAD" );
        if ( rc == 0 ) {
            /* get the actual commit structure */
            rc = git_commit_lookup ( &commit, git_repo, &oid_parent_commit );
            if ( rc == 0 ) {
                return commit;
            }
        }
        return NULL;
    }


    void repository_delete_file ( std::string path )
    {
        notes_print_info ( "Delete file: %s\n", path.c_str () );

        int rc = git_index_remove_bypath ( git_repo_index, path.c_str () );
        if ( rc != 0 ) {
            notes_print_error ( "Failed add changes to index.\n" );
        }
        git_changed = true;
    }
    void repository_stage_file ( std::string path )
    {
        notes_print_info ( "Staging file: %s\n", path.c_str () );

        int rc = git_index_add_bypath ( git_repo_index, path.c_str () );
        if ( rc != 0 ) {
            notes_print_error ( "Failed add changes to index.\n" );
        }
        git_changed = true;
    }

    bool repository_commit_changes ( )
    {
        git_oid       tree_oid;
        git_oid       oid_commit;
        git_tree      *tree_cmt;
        git_signature *sign;


        if ( git_signature_default ( &sign, git_repo ) == GIT_ENOTFOUND ) {
            git_signature_new ( (git_signature * *) &sign,
                                "NotesCC", "dummy@nothere", 123456789, 0 );
        }


        // Create a tree to commit.
        int rc = git_index_write_tree ( &tree_oid, git_repo_index );
        if ( rc == 0 ) {
            // Lookup the created tree,
            rc = git_tree_lookup ( &tree_cmt, git_repo, &tree_oid );
            if ( rc == 0 ) {
                // Get last commit.
                git_commit       *last_commit = repository_get_last_commit ( );
                // If no last commit, create root commit.
                int              entries = ( last_commit == nullptr ) ? 0 : 1;
                // Create new commit.
                const git_commit *commits[] = { last_commit };
                git_commit_create ( &oid_commit,
                                    git_repo,
                                    "HEAD",
                                    sign, sign,
                                    NULL,
                                    "NoteCC autocommit.\n",
                                    tree_cmt, entries, commits );
                git_tree_free ( tree_cmt );
                if ( last_commit ) {
                    git_commit_free ( last_commit );
                }
            }
        }
        git_signature_free ( sign );

        rc = git_index_write ( git_repo_index );
        if ( rc != 0 ) {
            notes_print_error ( "Failed to write index to disc.\n" );
            return false;
        }

        return true;
    }

    // TODO: Clean this (fetch & full )mess up.
    bool repository_fetch ()
    {
        git_remote *remote = nullptr;
        int        rc      = git_remote_load ( &remote, git_repo, "origin" );
        if ( rc != 0 ) {
            const git_error *e = giterr_last ();
            notes_print_error ( "Failed to load remote 'origin': %s\n", e->message );
            return false;
        }

        git_remote_callbacks callbacks = GIT_REMOTE_CALLBACKS_INIT;
        callbacks.credentials = cred_acquire_cb;
        git_remote_set_callbacks ( remote, &callbacks );

        rc = git_remote_fetch ( remote, NULL, NULL );
        if ( rc != 0 ) {
            const git_error *e = giterr_last ();
            if(e != nullptr) {
                notes_print_error ( "Failed to access remote 'origin': %s\n", e->message );
            }else {
                notes_print_error ( "Failed to access remote 'origin'.\n");
                notes_print_error ( "please check that fetching works.\n");
            }
            git_remote_free ( remote );
            return false;
        }
        git_remote_free ( remote );
        return true;
    }
    bool repository_push ()
    {
        notes_print_info("Connecting to 'origin'\n");
        git_remote *remote = nullptr;
        int        rc      = git_remote_load ( &remote, git_repo, "origin" );
        if ( rc != 0 ) {
            const git_error *e = giterr_last ();
            notes_print_error ( "Failed to load remote 'origin': %s\n", e->message );
            return false;
        }

        git_remote_callbacks callbacks = GIT_REMOTE_CALLBACKS_INIT;
        callbacks.credentials = cred_acquire_cb;
        git_remote_set_callbacks ( remote, &callbacks );

        rc = git_remote_connect ( remote, GIT_DIRECTION_PUSH );
        if ( rc != 0 ) {
            const git_error *e = giterr_last ();
            if(e != nullptr) {
                notes_print_error ( "Failed to access remote 'origin': %s\n", e->message );
            }else {
                notes_print_error ( "Failed to access remote 'origin'.\n");
                notes_print_error ( "please check that pushing works.\n");
            }
            git_remote_free ( remote );
            return false;
        }
        notes_print_info ( "Pushing data\n" );
        git_push *out;
        rc = git_push_new ( &out, remote );
        if ( rc != 0 ) {
            const git_error *e = giterr_last ();
            notes_print_error ( "Failed to setup push to remote 'origin': %s\n", e->message );
            git_remote_free ( remote );
            return false;
        }

        git_reference *ref;
        rc = git_repository_head ( &ref, git_repo );
        if ( rc != 0 ) {
            const git_error *e = giterr_last ();
            notes_print_error ( "Failed to get head: %s\n", e->message );
            git_remote_free ( remote );
            return false;
        }
        // Push to master branch.
        rc = git_push_add_refspec ( out, git_reference_name ( ref ) );
        git_reference_free ( ref );
        if ( rc != 0 ) {
            const git_error *e = giterr_last ();
            notes_print_error ( "Failed to add refspec push to remote 'origin': %s\n", e->message );
            git_remote_free ( remote );
            return false;
        }
        rc = git_push_finish ( out );
        if ( rc != 0 ) {
            const git_error *e = giterr_last ();
            notes_print_error ( "Failed to push to remote 'origin': %s\n", e->message );
            git_remote_free ( remote );
            return false;
        }

        if ( !git_push_unpack_ok ( out ) ) {
            notes_print_error ( "Failed to push to remote location.\n" );

            git_push_status_foreach ( out,
                                      [] ( const char *a, const char *b, __attribute__ ( ( unused ) ) void *data ) {
                                          notes_print_error ( "Failed to push ref: %s %s\n", a, b );
                                          return 0;
                                      }, nullptr );
        }
        git_push_free ( out );


        git_remote_disconnect ( remote );
        git_remote_free ( remote );

        notes_print_info ("Fetching from 'origin'\n");
        return repository_fetch ();
    }
    bool repository_pull ( )
    {
        // Commit changes if exists.
        if ( git_changed ) {
            notes_print_info ( "Commiting changes to git.\n" );
            repository_commit_changes ();
            git_changed = false;
        }
        notes_print_info("Fetching from 'origin'\n");
        if ( !repository_fetch () ) {
            return false;
        }

        notes_print_info("Validating merge\n");

        git_reference *headref = NULL;
        int           ret      = git_reference_lookup ( &headref, git_repo, "FETCH_HEAD" );
        if ( ret != 0 ) {
            const git_error *e = giterr_last ();
            notes_print_error ( "Error: %d/%d: %s\n", ret, e->klass, e->message );
            notes_print_error ( "Failed to look up references for merge head\n" );
            return false;
        }

        git_merge_preference_t pref;
        git_merge_analysis_t   analysis;
        ret = git_merge_analysis ( &analysis, &pref, git_repo, (const git_merge_head * *) &headref, 1 );

        if ( ret != 0 ) {
            const git_error *e = giterr_last ();
            notes_print_error ( "Error: %d/%d: %s\n", ret, e->klass, e->message );
            notes_print_error ( "Failed to analyze merge \n" );
            git_reference_free ( headref );
            return false;
        }

        if ( ( analysis & GIT_MERGE_ANALYSIS_FASTFORWARD ) > 0 ) {
            notes_print_info ( "Fast forward merge required.\n" );
            git_reference *ref, *href;


            ret = git_repository_head ( &href, git_repo );
            if ( ret != 0 ) {
                notes_print_error ( "Failed to get repository head\n" );
                git_reference_free ( headref );
                return false;
            }

            if ( git_reference_shorthand ( href ) == nullptr ) {
                notes_print_error ( "Failed to get current repository branch name\n" );
                git_reference_free ( headref );
                git_reference_free ( href );
                return false;
            }
            // Get branch
            git_branch_lookup ( &ref, git_repo, git_reference_shorthand ( href ), GIT_BRANCH_LOCAL );

            git_reference_free ( href );

            if ( !git_branch_is_head ( ref ) ) {
                notes_print_error ( "Branch head is not current head\n" );
                git_reference_free ( headref );
                return false;
            }

            git_reference *newf;
            git_object    *obj;
            ret = git_reference_peel ( &obj, headref, GIT_OBJ_COMMIT );
            if ( ret != 0 ) {
                const git_error *e = giterr_last ();
                notes_print_error ( "Error: %d/%d: %s\n", ret, e->klass, e->message );
                notes_print_error ( "Failed to fast-forward the master branch.\n" );
                git_reference_free ( ref );
                git_reference_free ( headref );
                return false;
            }

            const git_oid * commit_id = git_object_id ( obj );
            ret = git_reference_set_target ( &newf, ref, commit_id, NULL, "fast-forward" );
            git_reference_free ( ref );
            git_object_free ( obj );
            if ( ret != 0 ) {
                const git_error *e = giterr_last ();
                notes_print_error ( "Error: %d/%d: %s\n", ret, e->klass, e->message );
                notes_print_error ( "Failed to fast-forward the master branch.\n" );
                git_reference_free ( headref );
                return false;
            }

            // Update working tree.
            git_checkout_options checkout_opts = GIT_CHECKOUT_OPTIONS_INIT;
            checkout_opts.checkout_strategy = GIT_CHECKOUT_FORCE;
            ret                             = git_checkout_head ( git_repo, &checkout_opts );
            if ( ret != 0 ) {
                const git_error *e = giterr_last ();
                notes_print_error ( "Error: %d/%d: %s\n", ret, e->klass, e->message );
                notes_print_error ( "Failed to checkout latest changes into Work Tree.\n" );
                git_reference_free ( headref );
                return false;
            }
            // Clear old index.
            git_index_read ( git_repo_index, false );

            this->clear ();
            this->Load ();
        }
        else if ( ( analysis & GIT_MERGE_ANALYSIS_UP_TO_DATE ) > 0 ) {
            notes_print_info ( "No change required.\n");
        }
        else {
            notes_print_error ( "The repository cannot be fast forwarded, please do a merge first\n" );
            git_reference_free ( headref );
            return false;
        }

        git_reference_free ( headref );

        return true;
    }

    bool check_repository_state ()
    {
        // Check if it is in a sane state.
        int state = git_repository_state ( git_repo );
        if ( state != GIT_REPOSITORY_STATE_NONE ) {
            notes_print_error ( "The repository is not in a clean state.\n" );
            notes_print_error ( "Please resolve any outstanding issues first.\n" );
            return false;
        }
        // Check bare directory
        if ( git_repository_is_bare ( git_repo ) ) {
            notes_print_error ( "Bare repositories are not supported.\n" );
            return false;
        }

        // Check open changes.
        git_status_list    *gsl = nullptr;
        git_status_options opts = GIT_STATUS_OPTIONS_INIT;
        opts.version = GIT_STATUS_OPTIONS_VERSION;
        opts.show    = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
        opts.flags   = GIT_STATUS_OPT_INCLUDE_UNTRACKED;

        if ( git_status_list_new ( &gsl, git_repo, &opts ) != 0 ) {
            notes_print_error ( "Failed to get repository status.\n" );
            return false;
        }

        size_t i, maxi = git_status_list_entrycount ( gsl );
        bool   clean = true;
        int    mask  = 0;
        for ( i = 0; i < maxi; i++ ) {
            const git_status_entry *s = git_status_byindex ( gsl, i );
            if ( s->status != GIT_STATUS_CURRENT ) {
                clean = false;
                mask |= s->status;
            }
        }
        git_status_list_free ( gsl );

        if ( !clean ) {
            if ( ( mask & GIT_STATUS_WT_NEW ) == GIT_STATUS_WT_NEW ) {
                notes_print_warning ( "There are untracked files in your repository\n" );
            }
            else {
                notes_print_error ( "There are modified files in your repository\n" );
                notes_print_error ( "Please fix these and commit the changes\n" );
                return false;
            }
        }

        if ( git_repository_index ( &git_repo_index, git_repo ) != 0 ) {
            notes_print_error ( "Failed to read index from disc.\n" );
            return false;
        }
        return true;
    }



    void clear ()
    {
        // delete notes.
        for ( auto note : notes ) {
            if ( note != nullptr ) {
                delete note;
            }
        }
        notes.clear ();
        for ( auto project : child_projects ) {
            delete project;
        }
        child_projects.clear ();
        this->last_note_id = 0;
    }


    void print_projects ()
    {
        this->print ();
    }

    std::string get_path ()
    {
        return settings.get_repository ();
    }

    std::string get_relative_path ()
    {
        return "";
    }
    template < class T>
    void display_notes ( const T view_notes )
    {
        TableView view;

        // Add the columns
        view.add_column ( "ID", color_bold );
        view[0].set_left_align ();
        view.add_column ( "Rev.", color_blue );
        view[1].set_left_align ();
        view.add_column ( "Project", color_white_bold );
        view.add_column ( "Last edited", color_green );
        view.add_column ( "Description" );

        unsigned int row_index = 0;
        for ( auto note : view_notes ) {
            // Skip empty elements.
            if ( note == nullptr ) {
                continue;
            }
            view[0].set_value ( row_index, std::to_string ( note->get_id () ) );
            if ( note->get_revision () > 0 ) {
                view[1].set_value ( row_index, std::to_string ( note->get_revision () ) );
            }
            else{
                view[1].set_value ( row_index, "" );
            }
            view[2].set_value ( row_index, note->get_project_name () );
            view[3].set_value ( row_index, note->get_modtime () );
            view[4].set_value ( row_index, note->get_title () );
            view++;
            row_index++;
        }
        view.print ();
    }



    Note * get_note ( int &cargs, int argc, char ** argv )
    {
        if ( argc == 1 ) {
            // Get index.
            try {
                unsigned int nindex = std::stoul ( argv[0] );
                if ( nindex > 0 && nindex <= last_note_id && notes[nindex - 1] != nullptr ) {
                    cargs++;
                    return notes[nindex - 1];
                }
                notes_print_error ( "Invalid note id: %d\n", nindex );
            } catch ( ... ) {
            }
        }

        NotesFilter filter ( this->notes );
        for ( int iter = 0; iter < argc; iter++ ) {
            filter.add_filter ( argv[iter] );
            cargs++;
        }

        // Get filtered notes.
        auto fnotes = filter.get_filtered_notes ();
        if ( fnotes.size () == 0 ) {
            return nullptr;
        }

        // If one note is remaining, pick that one
        else if ( fnotes.size () == 1 ) {
            return *( fnotes.begin () );
        }

        while ( true ) {
            this->display_notes ( fnotes );

            char* resp = readline ( "Enter note id: " );
            if ( resp ) {
                // Quit
                if ( resp[0] == 'q' ) {
                    free ( resp );
                    return nullptr;
                }
                try {
                    unsigned int nindex = std::stoul ( resp );
                    if ( nindex > 0 && nindex <= last_note_id && this->notes[nindex - 1] != nullptr ) {
                        free ( resp );
                        return this->notes[nindex - 1];
                    }
                }catch ( ... ) { }

                notes_print_error ( "Invalid note id: %s\n", resp );
                free ( resp );
            }
        }
        return nullptr;
    }

    /**
     * @param argc Number of renaming commandline options.
     * @param argv Remaining commandline options.
     *
     * Edit a notes.
     *
     * @returns number of consumed commandline options.
     */
    int command_edit ( int argc, char ** argv )
    {
        int  cargs = 0;
        Note *note = this->get_note ( cargs, argc, argv );
        if ( note == nullptr ) {
            notes_print_error ( "No note selected\n" );
            return cargs;
        }
        // Edit the note.
        if ( note->edit () ) {
            // Commit the result.
            auto path = note->get_relative_path ();
            repository_stage_file ( path );
        }

        return cargs;
    }

    /**
     * @param argc Number of renaming commandline options.
     * @param argv Remaining commandline options.
     *
     * View a notes.
     *
     * @returns number of consumed commandline options.
     */
    int command_view ( int argc, char ** argv )
    {
        int  cargs = 0;
        Note *note = this->get_note ( cargs, argc, argv );
        if ( note == nullptr ) {
            notes_print_error ( "No note selected\n" );
            return cargs;
        }

        note->view ();
        return cargs;
    }

    /**
     * @param argc Number of renaming commandline options.
     * @param argv Remaining commandline options.
     *
     * Print the raw note to stdout.
     *
     * @returns number of consumed commandline options.
     */
    int command_cat ( int argc, char ** argv )
    {
        int  cargs = 0;
        Note *note = this->get_note ( cargs, argc, argv );
        if ( note == nullptr ) {
            notes_print_error ( "No note selected\n" );
            return cargs;
        }

        note->cat ();
        return cargs;
    }

    /**
     * @param argc Number of renaming commandline options.
     * @param argv Remaining commandline options.
     *
     * List the notes.
     *
     * @returns number of consumed commandline options.
     */
    int command_list ( int argc, char ** argv )
    {
        int         iter = 0;
        NotesFilter filter ( this->notes );

        for (; iter < argc; iter++ ) {
            filter.add_filter ( argv[iter] );
        }
        this->display_notes ( filter.get_filtered_notes () );
        return iter;
    }
    int command_export ( int argc, char **argv )
    {
        if ( argc != 3 ) {
            notes_print_error ( "Export requires three arguments: <note id> <type> <export_file>\n" );
            return 0;
        }
        unsigned int nindex = 0;
        try {
            nindex = std::stoul ( argv[0] );
        } catch ( ... ) {
        }
        if ( nindex < 1 || nindex > last_note_id || notes[nindex - 1] == nullptr ) {
            notes_print_error ( "Invalid note id: %d\n", nindex );
            return 3;
        }
        Note        *note = notes[nindex - 1];

        std::string export_path = argv[2];
        std::string format      = argv[1];

        notes_print_info ( "Exporting note %u to '%s'\n", note->get_id (), argv[2] );

        if ( format == "raw" ) {
            note->export_to_file_raw ( export_path );
        }
        else if ( format == "html" ) {
            note->export_to_file_html ( export_path );
        }
        else {
            notes_print_error ( "%s is an invalid export format. (raw and html are supported)\n",
                                format.c_str () );
        }
        return 3;
    }

    static int cred_acquire_cb ( git_cred** cred, const char*,
                                 const char *user, unsigned int, void* )
    {
        return git_cred_ssh_key_from_agent ( cred, user );
    }
private:

    int command_move ( int argc, char **argv )
    {
        int iter = 0;
        if ( argc < 2 ) {
            notes_print_error ( "Move requires two arguments: <note id>  <project path>\n" );
            return iter;
        }
        iter++;
        unsigned int nindex = 0;
        try {
            nindex = std::stoul ( argv[0] );
        } catch ( ... ) {
        }
        if ( nindex < 1 || nindex > last_note_id || notes[nindex - 1] == nullptr ) {
            notes_print_error ( "Invalid note id: %d\n", nindex );
            return iter;
        }
        Note *note = notes[nindex - 1];

        iter++;
        std::string name = argv[1];
        Project     *p   = this->get_or_create_project_from_name ( name );
        if ( p == nullptr ) {
            return iter;
        }
        if ( p == note->get_project () ) {
            notes_print_warning ( "Destination same as source: Ignoring.\n" );
            return iter;
        }
        std::string old_path = note->get_relative_path ();
        if ( note->move ( p ) ) {
            std::string new_path = note->get_relative_path ();
            this->repository_stage_file ( new_path );
            this->repository_delete_file ( old_path );
        }
        return iter;
    }

    static void command_projects_add_entry ( Project *p, TableView &view, unsigned int &row )
    {
        view[0].set_value ( row, p->get_name () );
        std::string nnotes =
            std::to_string ( p->get_num_notes () ) + "/" + std::to_string ( p->get_num_notes_recursive () );
        view[1].set_value ( row, nnotes );
        row++;
        view++;
        for ( auto pc : p->get_child_projects () ) {
            command_projects_add_entry ( pc, view, row );
        }
    }
    int command_projects ( __attribute__( ( unused ) ) int argc, __attribute__( ( unused ) ) char **argv )
    {
        TableView view;
        view.add_column ( "Project", color_blue );
        view.add_column ( "Num. Notes", color_white );
        view[1].set_left_align ();
        unsigned int row = 0;
        for ( auto p : this->get_child_projects () ) {
            command_projects_add_entry ( p, view, row );
        }
        view.print ();
        return 0;
    }

    void command_view_autocomplete ()
    {
        for ( auto note : notes ) {
            printf ( "%u\n", note->get_id () );
        }
    }

    void command_edit_autocomplete ()
    {
        for ( auto note : notes ) {
            printf ( "%u\n", note->get_id () );
        }
    }

    /**
     * Resolve a normal full project name into the Project pointer.
     * If (sub) projects do not exists, they are created and added.
     *
     * Before adding a note, validate if it exists by calling
     * Project::check_and_create_path().
     */
    Project * get_or_create_project_from_name ( std::string pr )
    {
        Project *p = this;
        if ( pr.empty () ) {
            return p;
        }
        // Split the string.
        size_t str_start = 0;
        while ( str_start < pr.size () ) {
            auto str_end = pr.find_first_of ( '.', str_start );
            if ( str_end == std::string::npos ) {
                str_end = pr.size ();
            }
            auto pr_name = pr.substr ( str_start, str_end - str_start );
            if ( !pr_name.empty () ) {
                // Validate the Project name only consists of characters and numbers.
                if ( find_if ( pr_name.begin (), pr_name.end (),
                               [] ( char c ) {
                                   return !( isalnum ( c ) );
                               } ) != pr_name.end () ) {
                    notes_print_error ( "%s is an invalid Project name.\n", pr_name.c_str () );
                    return nullptr;
                }
                Project *pc = p->find_child ( pr_name );
                if ( pc == nullptr ) {
                    // Create!
                    pc = new Project ( pr_name.c_str () );
                    p->add_subproject ( pc );
                }
                p = pc;
            }

            str_start = str_end + 1;
        }
        return p;
    }

    int command_delete ( int argc, char **argv )
    {
        int  cargs = 0;
        Note *note = this->get_note ( cargs, argc, argv );
        if ( note == nullptr ) {
            notes_print_error ( "No note selected\n" );
            return cargs;
        }

        // Delete the file from internal structure and working directory.
        unsigned int nindex = note->get_id () - 1;
        if ( note->del () ) {
            // Tell git the file is removed.
            repository_delete_file ( note->get_relative_path () );
            // Delete the entry from the list.
            delete note;
            notes[nindex] = nullptr;
        }
        return cargs;
    }
    int command_add ( int argc, char **argv )
    {
        int     retv = 0;
        Project *p   = this;

        if ( argc > 0 ) {
            p = this->get_or_create_project_from_name ( argv[0] );
            retv++;
        }

        // Check if we have project successful.
        if ( p == nullptr ) {
            notes_print_error ( "Failed to find or create the project.\n" );
            return retv;
        }

        Note *n = new Note ( p, &settings );

        if ( n != nullptr ) {
            this->notes.push_back ( n );
            n->set_id ( ++this->last_note_id );
            n->edit ();
            // Commit the result.
            auto path = n->get_relative_path ();
            repository_stage_file ( path );
        }

        return retv;
    }

    void command_export_autocomplete ( int argc, __attribute__( ( unused ) ) char **argv  )
    {
        if ( argc == 1 ) {
            for ( auto note : notes ) {
                printf ( "%u\n", note->get_id () );
            }
        }
        else if ( argc == 2 ) {
            printf ( "html\nraw\n" );
        }
    }
    void command_move_autocomplete ( int argc, __attribute__ ( ( unused ) ) char **argv )
    {
        if ( argc == 1 ) {
            for ( auto note : notes ) {
                printf ( "%u\n", note->get_id () );
            }
        }
        else if ( argc == 2 ) {
            list_projects ();
        }
    }

    /**
     * Implement the autocomplete command.
     */
    void run_autocomplete ( int argc, char **argv )
    {
        if ( argc == 1 ) {
            // List commands.
            for ( int i = 0; commands[i] != nullptr; i++ ) {
                std::cout << commands[i] << std::endl;
            }
            return;
        }
        std::string command = argv[1];
        if ( command == "view" || command == "cat" ) {
            if ( argc == 2 ) {
                this->command_view_autocomplete ();
            }
            return;
        }
        else if ( command == "export" ) {
            this->command_export_autocomplete ( argc - 1, &argv[1] );
        }
        else if ( command == "move" ) {
            this->command_move_autocomplete ( argc - 1, &argv[1] );
        }
        else if ( command == "edit"  ) {
            if ( argc == 2 ) {
                this->command_edit_autocomplete ();
            }
            return;
        }
        else if ( command == "add" ) {
            if ( argc == 2 ) {
                this->list_projects ();
            }
            return;
        }
    }

    /**
     * Interactive shell
     */
    void interactive ()
    {
        // Enable history.
        using_history ();

        do {
            // Create interactive prompt.
            char *temp = readline ( "$ " );

            // Quit on ctrl-d or quit.
            if ( temp == nullptr ) {
                break;
            }
            if ( strcasecmp ( temp, "quit" ) == 0 ) {
                free ( temp );
                break;
            }
            // Add entry to history.
            add_history ( temp );

            // Split into arc, argv structure.
            int  length     = strlen ( temp );
            int  argc       = 0;
            int  last_index = 0;
            char **argv     = nullptr;
            for ( int i = 0; i <= length; i++ ) {
                if ( temp[i] == ' ' || temp[i] == '\0' ) {
                    if ( i != last_index ) {
                        argv       = (char * *) realloc ( argv, sizeof ( char * ) * ( argc + 1 ) );
                        argv[argc] = &temp[last_index];
                        temp[i]    = '\0';
                        argc++;
                    }
                    last_index = i + 1;
                }
            }

            // Run parser.
            this->cmd_parser ( argc, argv );

            // Free
            free ( argv );
            free ( temp );
        } while ( true );
    }

    void cmd_parser ( int argc, char **argv )
    {
        int index = 0;
        while ( index < argc ) {
            if ( strcmp ( argv[index], "view" ) == 0 ) {
                index++;
                index += this->command_view ( argc - index, &argv[index] );
            }
            else if ( strcmp ( argv[index], "cat" ) == 0 ) {
                index++;
                index += this->command_cat ( argc - index, &argv[index] );
            }
            else if ( strcmp ( argv[index], "list" ) == 0 ) {
                index++;
                index += this->command_list ( argc - index, &argv[index] );
            }
            else if ( strcmp ( argv[index], "edit" ) == 0 ) {
                index++;
                index += this->command_edit ( argc - index, &argv[index] );
            }
            else if ( strcmp ( argv[index], "add" ) == 0 ) {
                index++;
                index += this->command_add ( argc - index, &argv[index] );
            }
            else if ( strcmp ( argv[index], "move" ) == 0 ) {
                index++;
                index += this->command_move ( argc - index, &argv[index] );
            }
            else if ( strcmp ( argv[index], "delete" ) == 0 ) {
                index++;
                index += this->command_delete ( argc - index, &argv[index] );
            }
            else if ( strcmp ( argv[index], "export" ) == 0 ) {
                index++;
                index += this->command_export ( argc - index, &argv[index] );
            }
            else if ( strcmp ( argv[index], "projects" ) == 0 ) {
                index++;
                index += this->command_projects ( argc - index, &argv[index] );
            }
            else if ( strcmp ( argv[index], "pull" ) == 0 ) {
                index++;
                repository_pull ();
            }
            else if ( strcmp ( argv[index], "push" ) == 0 ) {
                index++;
                repository_push ();
            }
            else {
                notes_print_error ( "Invalid command: '%s'\n", argv[index] );
                return;
            }
        }
        if ( argc == 0 ) {
            this->command_list ( 0, NULL );
        }
    }

    void Load ( )
    {
        // Iterate over all files in the git index.
        size_t i, maxi = git_index_entrycount ( git_repo_index );
        for ( i = 0; i < maxi; i++ ) {
            const git_index_entry *entry = git_index_get_byindex ( git_repo_index, i );
            // Do some path parsing magick.
            char                  *path = strdup ( entry->path );
            // Find filename.
            char                  *filename = nullptr;
            for ( int iter = strlen ( path ) - 1; iter >= 0 && path[iter] != '/'; iter-- ) {
                filename = &path[iter];
            }

            // Find project.
            Project *p = this;
            if ( filename != path ) {
                *( filename - 1 ) = '\0';
                for ( unsigned int iter = 0; iter < strlen ( path ); iter++ ) {
                    if ( path[iter] == '/' ) {
                        path[iter] = '.';
                    }
                }
                p = this->get_or_create_project_from_name ( path );
            }

            Note *note = new Note ( p, &settings, filename );
            // Add to the flat list in the main.
            this->notes.push_back ( note );
            free ( path );
        }
        // Sort the notes.
        std::sort ( this->notes.begin (), this->notes.end (), notes_print_sort );

        // Gives them UIDs.
        for ( auto note : this->notes ) {
            note->set_id ( ++this->last_note_id );
        }
    }
};


int main ( int argc, char ** argv )
{
    INIT_TIC_TAC ()

    NotesCC * notes = new NotesCC ( );

    // Open repository
    if ( notes->open_repository ( ) ) {
        notes->run ( argc, argv );
    }

    delete notes;

    TIC ( "Total runtime: " );
    return EXIT_SUCCESS;
}
