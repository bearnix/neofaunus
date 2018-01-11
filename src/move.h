#pragma once

#include "core.h"
#include "energy.h"
#include "average.h"
#include "analysis.h"
#include "potentials.h"

namespace Faunus {
    namespace Move {

        class Movebase {
            private:
                virtual void _move(Change&)=0; //!< Perform move and modify change object
                virtual void _accept(Change&) {}; //!< Call after move is accepted
                virtual void _reject(Change&) {}; //!< Call after move is rejected
                TimeRelativeOfTotal<std::chrono::microseconds> timer;
            protected:
                virtual void _to_json(json &j) const=0; //!< Extra info for report if needed
                virtual void _from_json(const json &j)=0; //!< Extra info for report if needed
                unsigned long cnt=0;
                unsigned long accepted=0;
                unsigned long rejected=0;
            public:
                static Random slump;   //!< Shared for all moves
                std::string name;      //!< Name of move
                int repeat=1;          //!< How many times the move should be repeated per sweep

                inline void from_json(const json &j) {
                    auto it = j.find("repeat");
                    if (it!=j.end()) {
                        if (it->is_number())
                            repeat = it->get<double>();
                        else
                            if (it->is_string())
                                if (it->get<std::string>()=="N")
                                    repeat = -1;
                    }
                    _from_json(j);
                    if (repeat<0)
                        repeat=0;
                }

                inline void to_json(json &j) const {
                    _to_json(j);
                    j["relative time"] = timer.result();
                    j["acceptance"] = double(accepted)/cnt;
                    j["repeat"] = repeat;
                    j["moves"] = cnt;
                    _roundjson(j, 3);
                } //!< JSON report w. statistics, output etc.

                inline void move(Change &change) {
                    timer.start();
                    cnt++;
                    change.clear();
                    _move(change);
                    if (change.empty())
                        timer.stop();
                } //!< Perform move and modify given change object

                inline void accept(Change &c) {
                    accepted++;
                    _accept(c);
                    timer.stop();
                }

                inline void reject(Change &c) {
                    rejected++;
                    _reject(c);
                    timer.stop();
                }
        };

        Random Movebase::slump; // static instance of Random (shared for all moves)

        inline void from_json(const json &j, Movebase &m) {
            m.from_json( j );
        } //!< Configure any move via json

        inline void to_json(json &j, const Movebase &m) {
            assert( !m.name.empty() );
            m.to_json(j[m.name]);
        }

        /**
         * @brief Translate and rotate a molecular group
         */
        template<typename Tspace>
            class AtomicTranslateRotate : public Movebase {
                private:
                    typedef typename Tspace::Tpvec Tpvec;
                    typedef typename Tspace::Tparticle Tparticle;
                    Tspace& spc; // Space to operate on
                    int molid=-1;
                    Point dir={1,1,1};
                    Average<double> msqd; // mean squared displacement
                    double _sqd; // squared displament
                    std::string molname; // name of molecule to operate on
                    Change::data cdata;

                    void _to_json(json &j) const override {
                        j = {
                            {"dir", dir},
                            {"molid", molid},
                            {u8::rootof + u8::bracket("r" + u8::squared), std::sqrt(msqd.avg())},
                            {"molecule", molname}
                        };
                        _roundjson(j,3);
                    }

                    void _from_json(const json &j) override {
                        assert(!molecules<Tpvec>.empty());
                        try {
                            molname = j.at("molecule");
                            auto it = findName(molecules<Tpvec>, molname);
                            if (it == molecules<Tpvec>.end())
                                throw std::runtime_error("unknown molecule '" + molname + "'");
                            molid = it->id();
                            dir = j.value("dir", Point(1,1,1));
                            if (repeat<0) {
                                auto v = spc.findMolecules(molid);
                                repeat = std::distance(v.begin(), v.end()); // repeat for each molecule...
                                if (repeat>0)
                                    repeat = repeat * v.front().size();     // ...and for each atom
                            }
                        }
                        catch (std::exception &e) {
                            std::cerr << name << ": " << e.what();
                            throw;
                        }
                    } //!< Configure via json object

                    typename Tpvec::iterator randomAtom() {
                        //auto p = spc.p.begin();
                        //auto g = spc.groups.begin();
                        //cdata.index = Faunus::distance( spc.groups.begin(), g ); // integer *index* of moved group
                        //cdata.atoms[0] = std::distance(g->begin(), p);  // index of particle rel. to group
                        //return p; 

                        assert(molid>=0);
                        auto mollist = spc.findMolecules( molid ); // all `molid` groups
                        if (size(mollist)>0) {
                            auto git = slump.sample( mollist.begin(), mollist.end() ); // random molecule iterator
                            if (!git->empty()) {
                                auto p = slump.sample( git->begin(), git->end() ); // random particle iterator  
                                cdata.index = Faunus::distance( spc.groups.begin(), git ); // integer *index* of moved group
                                cdata.atoms[0] = std::distance(git->begin(), p);  // index of particle rel. to group
                                return p; 
                            }
                        }
                        return spc.p.end();
                    }

                    void _move(Change &change) override {
                        auto p = randomAtom();
                        if (p!=spc.p.end()) {
                            double dp = atoms<Tparticle>.at(p->id).dp;
                            double dprot = atoms<Tparticle>.at(p->id).dprot;
                            auto& g = spc.groups[cdata.index];

                            if (dp>0) { // translate
                                Point oldpos = p->pos;
                                p->pos +=  0.5 * dp * ranunit(slump).cwiseProduct(dir);
                                spc.geo.boundaryFunc(p->pos);
                                _sqd = spc.geo.sqdist(oldpos, p->pos); // squared displacement
                                if (!g.atomic)
                                    g.cm = Geometry::massCenter(g.begin(), g.end(), spc.geo.boundaryFunc);
                            }

                            if (dprot>0) { // rotate
                                Point u = ranunit(slump);
                                double angle = dprot * (slump()-0.5);
                                Eigen::Quaterniond Q( Eigen::AngleAxisd(angle, u) );
                                p->rotate(Q, Q.toRotationMatrix());
                            }

                            if (dp>0 || dprot>0)
                                change.groups.push_back( cdata ); // add to list of moved groups
                        }
                        else
                            std::cerr << name << ": no atoms found" << std::endl;
                    }

                    void _accept(Change &change) override { msqd += _sqd; }
                    void _reject(Change &change) override { msqd += 0; }

                public:
                    AtomicTranslateRotate(Tspace &spc) : spc(spc) {
                        name = "transrot";
                        repeat = -1; // meaning repeat N times
                        cdata.atoms.resize(1);
                    }
            };

        /**
         * @brief Translate and rotate a molecular group
         */
        template<typename Tspace>
            class TranslateRotate : public Movebase {
                private:
                    typedef typename Tspace::Tpvec Tpvec;
                    Tspace& spc; // Space to operate on
                    int molid=-1;
                    double dptrans=0;
                    double dprot=0;
                    Point dir={1,1,1};
                    Average<double> msqd; // mean squared displacement
                    double _sqd; // squared displament

                    void _to_json(json &j) const override {
                        j = {
                            {"dir", dir}, {"dp", dptrans}, {"dprot", dprot},
                            {"molid", molid},
                            {u8::rootof + u8::bracket("r" + u8::squared), std::sqrt(msqd.avg())},
                            {"molecule", molecules<Tpvec>[molid].name}
                        };
                        _roundjson(j,3);
                    }

                    void _from_json(const json &j) override {
                        assert(!molecules<Tpvec>.empty());
                        try {
                            std::string molname = j.at("molecule");
                            auto it = findName(molecules<Tpvec>, molname);
                            if (it == molecules<Tpvec>.end())
                                throw std::runtime_error("unknown molecule '" + molname + "'");
                            molid = it->id();
                            dir = j.value("dir", Point(1,1,1));
                            dprot = j.at("dprot");
                            dptrans = j.at("dp");
                            if (repeat<0) {
                                auto v = spc.findMolecules(molid);
                                repeat = std::distance(v.begin(), v.end());
                            }
                        }
                        catch (std::exception &e) {
                            std::cerr << name << ": " << e.what();
                            throw;
                        }
                    } //!< Configure via json object

                    void _move(Change &change) override {
                        assert(molid>=0);
                        assert(!spc.groups.empty());
                        assert(spc.geo.getVolume()>0);

                        // pick random group from the system matching molecule type
                        // TODO: This can be slow -- implement look-up-table in Space
                        auto mollist = spc.findMolecules( molid ); // list of molecules w. 'molid'
                        if (size(mollist)>0) {
                            auto it = slump.sample( mollist.begin(), mollist.end() );
                            if (!it->empty()) {
                                assert(it->id==molid);

                                if (dptrans>0) { // translate
                                    Point oldcm = it->cm;
                                    Point dp = 0.5*ranunit(slump).cwiseProduct(dir) * dptrans;
                                    it->translate( dp, spc.geo.boundaryFunc );
                                    _sqd = spc.geo.sqdist(oldcm, it->cm); // squared displacement
                                }

                                if (dprot>0) { // rotate
                                    Point u = ranunit(slump);
                                    double angle = dprot * (slump()-0.5);
                                    Eigen::Quaterniond Q( Eigen::AngleAxisd(angle, u) );
                                    it->rotate(Q, spc.geo.boundaryFunc);
                                }

                                if (dptrans>0|| dprot>0) { // define changes
                                    Change::data d;
                                    d.index = Faunus::distance( spc.groups.begin(), it ); // integer *index* of moved group
                                    d.all = true; // *all* atoms in group were moved
                                    change.groups.push_back( d ); // add to list of moved groups
                                }
                                assert( spc.geo.sqdist( it->cm,
                                            Geometry::massCenter(it->begin(),it->end(),spc.geo.boundaryFunc,-it->cm) ) < 1e-9 );
                            }
                        }
                        else std::cerr << name << ": no molecules found" << std::endl;
                    }

                    void _accept(Change &change) override { msqd += _sqd; }
                    void _reject(Change &change) override { msqd += 0; }

                public:
                    TranslateRotate(Tspace &spc) : spc(spc) {
                        name = "moltransrot";
                        repeat = -1; // meaning repeat N times
                    }
            };

#ifdef DOCTEST_LIBRARY_INCLUDED
        TEST_CASE("[Faunus] TranslateRotate")
        {
            typedef Particle<Radius, Charge, Dipole, Cigar> Tparticle;
            typedef Space<Geometry::Cuboid, Tparticle> Tspace;
            typedef typename Tspace::Tpvec Tpvec;

            CHECK( !atoms<Tparticle>.empty() ); // set in a previous test
            CHECK( !molecules<Tpvec>.empty() ); // set in a previous test

            Tspace spc;
            TranslateRotate<Tspace> mv(spc);
            json j = R"( {"molecule":"B", "dp":1.0, "dprot":0.5, "dir":[0,1,0], "repeat":2 })"_json;
            mv.from_json(j);

            j = json(mv).at(mv.name);
            CHECK( j.at("molecule")   == "B");
            CHECK( j.at("dir")   == Point(0,1,0) );
            CHECK( j.at("dp")    == 1.0 );
            CHECK( j.at("repeat")  == 2 );
            CHECK( j.at("dprot") == 0.5 );
        }
#endif

        template<typename Tspace>
            class VolumeMove : public Movebase {
                private:
                    typedef typename Tspace::Tpvec Tpvec;
                    Tspace& spc;
                    Average<double> msqd; // mean squared displacement
                    double dV=0, deltaV=0;

                    void _to_json(json &j) const override {
                        using namespace u8;
                        j = {
                            {"dV", dV},
                            {rootof + bracket(Delta + "V" + squared), std::sqrt(msqd.avg())},
                            {cuberoot + rootof + bracket(Delta + "V" + squared),
                                std::cbrt(std::sqrt(msqd.avg()))}
                        };
                        _roundjson(j,3);
                    }

                    void _from_json(const json &j) override {
                        dV = j.at("dV");
                    } //!< Configure via json object

                    void _move(Change &change) override {
                        if (dV>0) {
                            change.dV=true;
                            change.all=true;
                            double Vold = spc.geo.getVolume();
                            double Vnew = std::exp(std::log(Vold) + (slump()-0.5) * dV);
                            double scale = std::cbrt(Vnew/Vold);
                            deltaV = Vnew-Vold;
                            spc.scaleVolume(Vnew);
                        }
                    }

                    void _accept(Change &change) override { msqd += deltaV*deltaV; }
                    void _reject(Change &change) override { msqd += 0; }

                public:
                    VolumeMove(Tspace &spc) : spc(spc) {
                        name = "volume";
                        repeat = 1;
                    }
            };

        template<typename Tspace>
            class Pivot : public Movebase {
                private:
                    typedef typename Tspace::Tpvec Tpvec;
                    std::vector<std::reference_wrapper<const Potential::BondData>> bonds;
                    std::vector<int> index; // atom index to rotate
                    Tspace& spc;
                    std::string molname;
                    int molid;
                    double dprot;
                    double d2; // cm movement, squared
                    Average<double> msqd; // cm mean squared displacement

                    void _to_json(json &j) const override {
                        using namespace u8;
                        j = {
                            {"molecule", molname}, {"dprot", dprot},
                            {u8::rootof + u8::bracket("r_cm" + u8::squared), std::sqrt(msqd.avg())}
                        };
                        _roundjson(j,3);
                    }

                    void _from_json(const json &j) override {
                        dprot = j.at("dprot");
                        molname = j.at("molecule");
                        auto it = findName(molecules<Tpvec>, molname);
                        if (it == molecules<Tpvec>.end())
                            throw std::runtime_error("unknown molecule '" + molname + "'");
                        molid = it->id();
                        bonds = Potential::filterBonds(
                                molecules<Tpvec>[molid].bonds, Potential::BondData::harmonic);
                        if (repeat<0) {
                            auto v = spc.findMolecules(molid);
                            repeat = std::distance(v.begin(), v.end()); // repeat for each molecule...
                            if (repeat>0)
                                repeat *= bonds.size();
                        }
                    }

                    void _move(Change &change) override {
                        d2=0;
                        if (std::fabs(dprot)>1e-9) {
                            auto it = spc.randomMolecule(molid, slump);
                            if (it!=spc.groups.end())
                                if (it->size()>2) {
                                    auto b = slump.sample(bonds.begin(), bonds.end()); // random harmonic bond
                                    if (b != bonds.end()) {
                                        int i1 = b->get().index.at(0);
                                        int i2 = b->get().index.at(1);
                                        int offset = std::distance( spc.p.begin(), it->begin() );

                                        index.clear();
                                        if (slump()>0.0)
                                            for (size_t i=i2+1; i<it->size(); i++)
                                                index.push_back(i+offset);
                                        else
                                            for (size_t i=0; i<i1; i++)
                                                index.push_back(i+offset);
                                        i1+=offset;
                                        i2+=offset;

                                        if (!index.empty()) {
                                            Point oldcm = it->cm;
                                            it->unwrap(spc.geo.distanceFunc); // remove pbc
                                            Point u = (spc.p[i1].pos - spc.p[i2].pos).normalized();
                                            double angle = dprot * (slump()-0.5);
                                            Eigen::Quaterniond Q( Eigen::AngleAxisd(angle, u) );
                                            auto M = Q.toRotationMatrix();
                                            for (auto i : index) {
                                                spc.p[i].rotate(Q, M); // internal rot.
                                                spc.p[i].pos = Q * ( spc.p[i].pos - spc.p[i1].pos)
                                                    + spc.p[i1].pos; // positional rot.
                                            }
                                            it->cm = Geometry::massCenter(it->begin(), it->end());
                                            it->wrap(spc.geo.boundaryFunc); // re-apply pbc

                                            d2 = spc.geo.sqdist(it->cm, oldcm); // CM movement

                                            Change::data d;
                                            d.index = Faunus::distance( spc.groups.begin(), it ); // integer *index* of moved group
                                            d.all = true; // *all* atoms in group were moved
                                            change.groups.push_back( d ); // add to list of moved groups
                                        }
                                    }
                                }
                        }
                    }

                    void _accept(Change &change) override { msqd += d2; }
                    void _reject(Change &change) override { msqd += 0; }

                public:
                    Pivot(Tspace &spc) : spc(spc) {
                        name = "pivot";
                        repeat = -1; // --> repeat=N
                    }
            }; //!< Pivot move around random harmonic bond axis

        template<typename Tspace>
            class Propagator : public BasePointerVector<Movebase> {
                private:
                    int _repeat;
                    std::discrete_distribution<> dist;
                    std::vector<double> w; // list of weights for each move

                    void addWeight(double weight=1) {
                        w.push_back(weight);
                        dist = std::discrete_distribution<>(w.begin(), w.end());
                        _repeat = int(std::accumulate(w.begin(), w.end(), 0.0));
                    }

                public:
                    using BasePointerVector<Movebase>::vec;
                    inline Propagator() {}
                    inline Propagator(const json &j, Tspace &spc) {
                        for (auto &m : j.at("moves")) {// loop over move list
                            size_t oldsize = vec.size();
                            for (auto it=m.begin(); it!=m.end(); ++it) {
                                try {

                                    if (it.key()=="moltransrot") {
                                        this->template push_back<Move::TranslateRotate<Tspace>>(spc);
                                        vec.back()->from_json( it.value() );
                                    }

                                    if (it.key()=="transrot") {
                                        this->template push_back<Move::AtomicTranslateRotate<Tspace>>(spc);
                                        vec.back()->from_json( it.value() );
                                    }

                                    if (it.key()=="pivot") {
                                        this->template push_back<Move::Pivot<Tspace>>(spc);
                                        vec.back()->from_json( it.value() );
                                    }

                                    if (it.key()=="volume") {
                                        this->template push_back<Move::VolumeMove<Tspace>>(spc);
                                        vec.back()->from_json( it.value() );
                                    }

                                    // additional moves go here...

                                    if (vec.size()==oldsize+1)
                                        addWeight(vec.back()->repeat);
                                    else
                                        std::cerr << "warning: ignoring unknown move '" << it.key() << "'" << endl;
                                } catch (std::exception &e) {
                                    throw std::runtime_error("Error adding move '" + it.key() + "': " + e.what());
                                }
                            }
                        }
                    }

                    int repeat() { return _repeat; }

                    auto sample() {
                        if (!vec.empty()) {
                            assert(w.size() == vec.size());
                            return vec.begin() + dist( Move::Movebase::slump.engine );
                        }
                        return vec.end();
                    } //!< Pick move from a weighted, random distribution
            };

    }//Move namespace

    template<class Tgeometry, class Tparticle>
        class MCSimulation {
            private:
                typedef Space<Tgeometry, Tparticle> Tspace;
                typedef typename Tspace::Tpvec Tpvec;

                bool metropolis(double du) const {
                    if (std::isnan(du))
                        return false;
                    if (du<0)
                        return true;
                    return ( Move::Movebase::slump() > std::exp(-du)) ? false : true;
                } //!< Metropolis criterion (true=accept)

                struct State {
                    Tspace spc;
                    Energy::Hamiltonian<Tspace> pot;
                    State(const json &j) : spc(j), pot(spc,j) {}

                    void sync(State &other, Change &change) {
                        spc.sync( other.spc, change );
                        // sync energy here...
                    }
                }; //!< Contains everything to describe a state

                State state1, state2;
                double uinit=0, dusum=0;
                Average<double> uavg;

            public:
                Move::Propagator<Tspace> moves;

                auto& pot() { return state1.pot; }
                auto& space() { return state1.spc; }
                const auto& pot() const { return state1.pot; }
                const auto& space() const { return state1.spc; }
                const auto& geometry() const { return state1.spc.geo; }
                const auto& particles() const { return state1.spc.p; }

                double drift() {
                    Change c;
                    c.all=true;
                    double ufinal = state1.pot.energy(c);
                    return ( ufinal-(uinit+dusum) ) / uinit; 
                } //!< Calculates the relative energy drift from initial configuration

                MCSimulation(const json &j) : state1(j), state2(j), moves(j, state2.spc) {
                    Change c;
                    c.all=true;
                    state2.sync(state1, c);
                }

                void restore(const json &j) {
                    dusum=0;
                    Change c;
                    c.all=true;
                    state1.spc = j;
                    state2.spc.sync(state1.spc, c);
                    uinit = state1.pot.energy(c);
                    assert(uinit == state2.pot.energy(c));
                } //!< restore system from previously saved state

                void move() {
                    Change change;
                    for (int i=0; i<moves.repeat(); i++) {
                        auto mv = moves.sample(); // pick random move
                        if (mv != moves.end() ) {

                            change.clear();
                            (**mv).move(change);

                            if (!change.empty()) {
                                double unew, uold, du;
#pragma omp parallel sections
                                {
#pragma omp section
                                    { unew = state2.pot.energy(change); }
#pragma omp section
                                    { uold = state1.pot.energy(change); }
                                }
                                du = unew - uold;
                                if ( metropolis(du+change.du) ) { // accept move
                                    state1.sync( state2, change );
                                    (**mv).accept(change);
                                }
                                else { // reject move
                                    state2.sync( state1, change );
                                    (**mv).reject(change);
                                    du=0;
                                }
                                dusum+=du; // sum of all energy changes
                            }
                        }
                    }
                }

                void to_json(json &j) {
                    j = state1.spc.info();
                    j["temperature"] = pc::temperature / 1.0_K;
                    j["moves"] = moves;
                    j["energy"].push_back(state1.pot);
                }
        };

    template<class Tgeometry, class Tparticle>
        void to_json(json &j, MCSimulation<Tgeometry,Tparticle> &mc) { mc.to_json(j); }

}//namespace
