#ifndef ProjectClang_h
#define ProjectClang_h

#include "Project.h"
#include "StringMap.h"
#include <clang-c/Index.h>
#include <rct/EventReceiver.h>
#include <rct/LinkedList.h>
#include <rct/Map.h>
#include <rct/Mutex.h>
#include <rct/String.h>

class ClangUnit;

typedef Map<uint32_t, Set<Location> > UsrSet;
typedef Map<uint32_t, Set<uint32_t> > DependSet;
typedef Map<uint32_t, Set<uint32_t> > VirtualSet;

struct CursorInfo
{
    uint32_t usr;
    int start, end;
    Project::Cursor::Kind kind;

    int length() const { return end - start; }
};

inline Log operator<<(Log log, const CursorInfo &c)
{
    log << String::format<64>("Usr: %d Range: %d-%d kind: %s", c.usr, c.start, c.end, Project::Cursor::kindToString(c.kind));
    return log;
}

template <> inline Serializer &operator<<(Serializer &s, const CursorInfo &b)
{
    s << b.usr << b.start << b.end << static_cast<uint32_t>(b.kind);
    return s;
}

template <> inline Deserializer &operator>>(Deserializer &s, CursorInfo &b)
{
    uint32_t kind;
    s >> b.usr >> b.start >> b.end >> kind;
    b.kind = static_cast<Project::Cursor::Kind>(kind);
    return s;
}

struct FixIt
{
    inline FixIt(uint32_t s = 0, uint32_t e = 0, const String &t = String())
        : start(s), end(e), text(t)
    {
    }
    inline bool operator<(const FixIt &other) const
    {
        return start < other.start;
    }
    inline bool operator==(const FixIt &other) const
    {
        return (start == other.start && end == other.end && text == other.text);
    }

    uint32_t start, end;
    String text;
};

class ClangCompletionJob;
class ClangParseJob;
struct ClangIndexInfo;

class ClangProject : public Project
{
public:
    ClangProject(const Path &path);
    virtual ~ClangProject();

    using Project::save;
    virtual bool save(Serializer &serializer);
    virtual bool restore(Deserializer &deserializer);

    virtual Cursor cursor(const Location &location) const;
    virtual void references(const Location& location, unsigned queryFlags,
                            const List<Path> &pathFilter, Connection *conn) const;
    virtual void status(const String &query, Connection *conn, unsigned queryFlags) const;
    virtual void dump(const SourceInformation &sourceInformation, Connection *conn) const;
    using Project::index;
    virtual void index(const SourceInformation &sourceInformation, Type type);
    virtual void remove(const Path &sourceFile);
    virtual bool isIndexing() const;

    virtual Set<Path> dependencies(const Path &path, DependencyMode mode) const;

    virtual Set<Path> files(int mode = AllFiles) const;
    virtual Set<String> listSymbols(const String &string, const List<Path> &pathFilter) const;
    virtual Set<Cursor> findCursors(const String &string, const List<Path> &pathFilter) const;
    virtual Set<Cursor> cursors(const Path &path) const;
    virtual bool codeCompleteAt(const Location &location, const String &source, Connection *conn);
    virtual String fixits(const Path &path) const;
    virtual void dirty(const Set<Path> &files);

    static LockingStringMap& usrMap() { return umap; }

private:
    char locationType(const Location& location) const;
    void writeReferences(const uint32_t usr, const Set<uint32_t>& pathSet, Connection* conn, unsigned keyFlags) const;
    void writeDeclarations(const uint32_t usr, const Set<uint32_t>& pathSet, Connection* conn, unsigned keyFlags) const;
    void onConnectionDestroyed(Connection *conn);
    void onCompletionFinished(ClangCompletionJob *job);
    void onCompletion(ClangCompletionJob *job, String completion, String signature);

    void dirtyUsrs();
    void dirtyDeps(uint32_t fileId);

    void jobFinished(const shared_ptr<ClangParseJob>& job);
    void sync(const shared_ptr<ClangParseJob>& currentJob);
    void syncJob(const shared_ptr<ClangParseJob>& job);

private:
    Map<uint32_t, ClangUnit*> units;
    CXIndex cidx;
    CXIndexAction caction;

    mutable Mutex mutex;
    int pendingJobs, jobsProcessed;
    StopWatch timer;
    Map<Location, uint32_t> incs;
    DependSet depends, reverseDepends;
    Map<String, Set<uint32_t> > names; // name->usr
    Map<Location, CursorInfo> usrs;    // location->usr
    UsrSet decls, defs, refs;          // usr->locations
    VirtualSet virtuals;               // usr->usrs
    Map<Path, Set<FixIt> > fixIts;
    Set<Path> dirtyFiles;

    List<shared_ptr<ClangParseJob> > syncJobs;

    static LockingStringMap umap;

    Map<ClangCompletionJob *, Connection*> mCompletions;

    friend class ClangUnit;
    friend class ClangParseJob;
};

#ifdef CLANG_CAN_REPARSE
class UnitCache
{
public:
    enum { MaxSize = 5 };

    class Unit
    {
    public:
        Unit(CXTranslationUnit u) : unit(u) { }
        ~Unit() { clang_disposeTranslationUnit(unit); }

        bool operator<(const Unit& other) const { return unit < other.unit; }

        CXTranslationUnit unit;

    private:
        Unit(const Unit& other);
        Unit& operator=(const Unit& other);
    };

    static void add(const Path& path, CXTranslationUnit unit)
    {
        shared_ptr<Unit> u(new Unit(unit));
        put(path, u);
    }

    static shared_ptr<Unit> get(const Path& path)
    {
        MutexLocker locker(&mutex);
        LinkedList<std::pair<Path, shared_ptr<Unit> > >::iterator it = units.begin();
        const LinkedList<std::pair<Path, shared_ptr<Unit> > >::const_iterator end = units.end();
        while (it != end) {
            if (it->first == path) {
                shared_ptr<Unit> copy = it->second;
                units.erase(it);
                return copy;
            }
            ++it;
        }
        return shared_ptr<Unit>();
    }

    static void put(const Path& path, const shared_ptr<Unit>& unit)
    {
        MutexLocker locker(&mutex);
        assert(path.isAbsolute());
        units.push_back(std::make_pair(path, unit));
        if (units.size() > MaxSize)
            units.pop_front();
    }

    static List<Path> paths()
    {
        MutexLocker lock(&mutex);
        List<Path> ret;
        for (LinkedList<std::pair<Path, shared_ptr<Unit> > >::const_iterator it = units.begin(); it != units.end(); ++it) {
            ret.append(it->first);
        }

        return ret;
    }

private:
    static Mutex mutex;
    static LinkedList<std::pair<Path, shared_ptr<Unit> > > units;
};
#endif // CLANG_CAN_REPARSE

#endif
