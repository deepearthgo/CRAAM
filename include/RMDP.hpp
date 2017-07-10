#pragma once

#include "State.hpp"

#include <vector>
#include <istream>
#include <fstream>
#include <memory>
#include <tuple>
#include <cassert>
#include <limits>
#include <algorithm>
#include <string>
#include <sstream>
#include <utility>
#include <iostream>

#include <boost/numeric/ublas/matrix.hpp>
#include <boost/numeric/ublas/vector.hpp>
#include <boost/numeric/ublas/lu.hpp>

#include "cpp11-range-master/range.hpp"
#ifdef IS_DEBUG
#include <boost/numeric/ublas/io.hpp>
#endif


/** \mainpage

Introduction
------------

Craam is a C++ library for solving *plain*, *robust*, or *optimistic* Markov decision processes. The library also provides basic tools that enable simulation and construction of MDPs from samples. There is also support for state aggregation and abstraction solution methods.

The library supports standard finite or infinite horizon discounted MDPs [Puterman2005]. Some basic stochastic shortest path methods are also supported. The library assumes *maximization* over actions. The states and actions must be finite.

The robust model extends the regular MDPs [Iyengar2005]. The library allows to model uncertainty in *both* the transitions and rewards, unlike some published papers on this topic. This is modeled by adding an outcome to each action. The outcome is assumed to be minimized by nature, similar to [Filar1997].

In summary, the MDP problem being solved is:

\f[v(s) = \max_{a \in \mathcal{A}} \min_{o \in \mathcal{O}} \sum_{s\in\mathcal{S}} ( r(s,a,o,s') + \gamma P(s,a,o,s') v(s') ) ~.\f]

Here, \f$\mathcal{S}\f$ are the states, \f$\mathcal{A}\f$ are the actions, \f$\mathcal{O}\f$ are the outcomes.

Available algorithms are *value iteration* and *modified policy iteration*. The library support both the plain worst-case outcome method and a worst case with respect to a base distribution.

Installation and Build Instruction
----------------------------------

See the README.rst

Getting Started
---------------

The main interface to the library is through the templated class GRMDP. The templated version of this class enable different definitions of the uncertainty set. The available specializations are:

- craam::MDP : plain MDP with no explicit definition of uncertainty
- craam::RMDP_L1 : a robust/uncertain with discrete outcomes with L1 constraints on the uncertainty


States, actions, and outcomes are identified using 0-based contiguous indexes. The actions are indexed independently for each states and the outcomes are indexed independently for each state and action pair.

Transitions are added through function add_transition. New states, actions, or outcomes are automatically added based on the new transition. Main supported solution methods are:

| Method                  |  Algorithm     |
| ----------------------- | ----------------
| GRMDP::vi_gs            | Gauss-Seidel value iteration; runs in a single thread.
| GRMDP::vi_jac           | Jacobi value iteration; parallelized using OpenMP.
| GRMDP::mpi_jac          | Jacobi modified policy iteration; parallelized with OpenMP. Generally, modified policy iteration is vastly more efficient than value iteration.
| GRMDP::vi_jac_fix       | Jacobi value iteration for policy evaluation; parallelized with OpenMP.

Each of the methods above supports average, robust, and optimistic computation modes for the Nature.

The following is a simple example of formulating and solving a small MDP.

\code

    #include "RMDP.hpp"
    #include "modeltools.hpp"

    #include <iostream>
    #include <vector>

    using namespace craam;
    using namespace std;

    int main(){
        MDP mdp(3);

        // transitions for action 0
        add_transition(mdp,0,0,0,1,0);
        add_transition(mdp,1,0,0,1,1);
        add_transition(mdp,2,0,1,1,1);

        // transitions for action 1
        add_transition(mdp,0,1,1,1,0);
        add_transition(mdp,1,1,2,1,0);
        add_transition(mdp,2,1,2,1,1.1);

        // solve using Jacobi value iteration
        auto&& re = mdp.mpi_jac(Uncertainty::Average,0.9);

        for(auto v : re.valuefunction){
            cout << v << " ";
        }

        return 0;
    }

\endcode

To compile the file, run:

\code{.sh}
     $ g++ -fopenmp -std=c++14 -I<path_to_RAAM.hpp> -L <path_to_libcraam.a> simple.cpp -lcraam
\endcode

Notice that the order of the arguments matters (`-lcraam` must follow the file name).

Also note that the library first needs to be build. See the README file for the instructions.

Common Use Cases
----------------

1. Formulate an uncertain MDP
2. Compute a solution to an uncertain MDP
3. Compute value of a fixed policy
4. Compute an occupancy frequency
5. Simulate transitions of an MDP
6. Construct MDP from samples
7. Simulate a general domain


General Assumptions
-------------------

- Transition probabilities must be non-negative but do not need to add up to one
- Transitions with 0 probabilities may be omitted, except there must be at least one target state in each transition
- **State with no actions**: A terminal state with value 0
- **Action with no outcomes**: Terminates with an error
- **Outcome with no target states**: Terminates with an error


References
----------

[Filar1997] Filar, J., & Vrieze, K. (1997). Competitive Markov decision processes. Springer.

[Puterman2005] Puterman, M. L. (2005). Markov decision processes: Discrete stochastic dynamic programming. Handbooks in operations research and management …. John Wiley & Sons, Inc.

[Iyengar2005] Iyengar, G. N. G. (2005). Robust dynamic programming. Mathematics of Operations Research, 30(2), 1–29.

[Petrik2014] Petrik, M., Subramanian S. (2014). RAAM : The benefits of robustness in approximating aggregated MDPs in reinforcement learning. In Neural Information Processing Systems (NIPS).

[Petrik2016] Petrik, M., & Luss, R. (2016). Interpretable Policies for Dynamic Product Recommendations. In Uncertainty in Artificial Intelligence (UAI).
*/


/// Main namespace which includes modeling a solving functionality
namespace craam {

using namespace std;
using namespace boost::numeric;
using namespace util::lang;

// **************************************************************************************
//  Generic MDP Class
// **************************************************************************************

/**
A general robust Markov decision process. Contains methods for constructing and solving RMDPs.

Some general assumptions (may depend on the state and action classes):
    - Transition probabilities must be non-negative but do not need to add
        up to a specific value
    - Transitions with 0 probabilities may be omitted, except there must
        be at least one target state in each transition
    - State with no actions: A terminal state with value 0
    - Action with no outcomes: Terminates with an error for uncertain models, but
                               assumes 0 return for regular models.
    - Outcome with no target states: Terminates with an error
    - Invalid actions are ignored
    - Behavior for a state with all invalid actions is not defined

\tparam SType Type of state, determines s-rectangularity or s,a-rectangularity and
        also the type of the outcome and action constraints
 */
template<class SType>
class GRMDP{
protected:
    /** Internal list of states */
    vector<SType> states;

public:

    /** Action identifier in a policy. Copies type from state type. */
    typedef typename SType::ActionId ActionId;
    /** Action identifier in a policy. Copies type from state type. */
    typedef typename SType::OutcomeId OutcomeId;

    /** Decision-maker's policy: Which action to take in which state.  */
    typedef vector<ActionId> ActionPolicy;
    /** Nature's policy: Which outcome to take in which state.  */
    typedef vector<OutcomeId> OutcomePolicy;

    /**
    Constructs the RMDP with a pre-allocated number of states. All
    states are initially terminal.
    \param state_count The initial number of states, which dynamically
                        increases as more transitions are added. All initial
                        states are terminal.
    */
    GRMDP(long state_count) : states(state_count){};

    /** Constructs an empty RMDP. */
    GRMDP(){};

    /**
    Assures that the MDP state exists and if it does not, then it is created.
    States with intermediate ids are also created
    \return The new state
    */
    SType& create_state(long stateid){
        assert(stateid >= 0);
        if(stateid >= (long) states.size())
            states.resize(stateid + 1);
        return states[stateid];
    }

    /**
    Creates a new state at the end of the states
    \return The new state
    */
    SType& create_state(){ return create_state(states.size());};

    /** Number of states */
    size_t state_count() const {return states.size();};

    /** Number of states */
    size_t size() const {return state_count();};

    /** Retrieves an existing state */
    const SType& get_state(long stateid) const {
        assert(stateid >= 0 && size_t(stateid) < state_count());
        return states[stateid];};

    /** Retrieves an existing state */
    const SType& operator[](long stateid) const {return get_state(stateid);};


    /** Retrieves an existing state */
    SType& get_state(long stateid) {
        assert(stateid >= 0 && size_t(stateid) < state_count());
        return states[stateid];};

    /** Retrieves an existing state */
    SType& operator[](long stateid){return get_state(stateid);};

    /** \returns list of all states */
    const vector<SType>& get_states() const {return states;};

    /**
    Check if all transitions in the process sum to one.
    Note that if there are no actions, or no outcomes for a state,
    the RMDP still may be normalized.
    \return True if and only if all transitions are normalized.
     */
    bool is_normalized() const{
        for(auto const& s : states){
            for(auto const& a : s.get_actions()){
                for(auto const& t : a.get_outcomes()){
                    if(!t.is_normalized()) return false;
        } } }
        return true;
    }

    /** Normalize all transitions to sum to one for all states, actions, outcomes. */
    void normalize(){
        for(SType& s : states)
            s.normalize();
    }

    /**
    Computes occupancy frequencies using matrix representation of transition
    probabilities. This method may not scale well

    \param init Initial distribution (alpha)
    \param discount Discount factor (gamma)
    \param policy Policy of the decision maker
    \param nature Policy of nature
    */
    numvec ofreq_mat(const Transition& init, prec_t discount,
                     const ActionPolicy& policy, const OutcomePolicy& nature) const{
        const auto n = state_count();

        // initial distribution
        auto&& initial_svec = init.probabilities_vector(n);
        ublas::vector<prec_t> initial_vec(n);
        // TODO: this is a wasteful copy operation
        copy(initial_svec.begin(), initial_svec.end(), initial_vec.data().begin());

        // get transition matrix
        unique_ptr<ublas::matrix<prec_t>> t_mat(transition_mat_t(policy,nature));

        // construct main matrix
        (*t_mat) *= -discount;
        (*t_mat) += ublas::identity_matrix<prec_t>(n);

        // solve set of linear equations
        ublas::permutation_matrix<prec_t> P(n);
        ublas::lu_factorize(*t_mat,P);
        ublas::lu_substitute(*t_mat,P,initial_vec);

        // copy the solution back to a vector
        copy(initial_vec.begin(), initial_vec.end(), initial_svec.begin());

        return initial_svec;
    }

    /**
    Constructs the rewards vector for each state for the RMDP.
    \param policy Policy of the decision maker
    \param nature Policy of nature
     */
    numvec rewards_state(const ActionPolicy& policy, const OutcomePolicy& nature) const{
        const auto n = state_count();
        numvec rewards(n);

        #pragma omp parallel for
        for(size_t s=0; s < n; s++){
            const SType& state = get_state(s);
            if(state.is_terminal())
                rewards[s] = 0;
            else
                rewards[s] = state.mean_reward(policy[s], nature[s]);
        }
        return rewards;
    }

    /**
    Checks if the policy and nature's policy are both correct.
    Action and outcome can be arbitrary for terminal states.
    \return If incorrect, the function returns the first state with an incorrect
            action and outcome. Otherwise the function return -1.
    */
    long is_policy_correct(const ActionPolicy& policy,
                           const OutcomePolicy& natpolicy) const{
        for(auto si : indices(states) ){
            // ignore terminal states
            if(states[si].is_terminal())
                continue;

            // call function of the state
            if(!states[si].is_action_outcome_correct(policy[si], natpolicy[si]))
                return si;
        }
        return -1;
    }

    
    /**
    Constructs the transition matrix for the policy.
    \param policy Policy of the decision maker
    \param nature Policy of the nature
    */
    unique_ptr<ublas::matrix<prec_t>>
        transition_mat(const ActionPolicy& policy,
                       const OutcomePolicy& nature) const{
        const size_t n = state_count();
        unique_ptr<ublas::matrix<prec_t>> result(new ublas::matrix<prec_t>(n,n));
        *result = ublas::zero_matrix<prec_t>(n,n);

        #pragma omp parallel for
        for(size_t s=0; s < n; s++){
            const Transition&& t = states[s].mean_transition(policy[s],nature[s]);
            const auto& indexes = t.get_indices();
            const auto& probabilities = t.get_probabilities();

            for(size_t j=0; j < t.size(); j++){
                (*result)(s,indexes[j]) = probabilities[j];
            }
        }
        return result;
    }

    /**
    Constructs a transpose of the transition matrix for the policy.
    \param policy Policy of the decision maker
    \param nature Policy of the nature
    */
    unique_ptr<ublas::matrix<prec_t>>
        transition_mat_t(const ActionPolicy& policy,
                         const OutcomePolicy& nature) const{
    
        const size_t n = state_count();
        unique_ptr<ublas::matrix<prec_t>> result(new ublas::matrix<prec_t>(n,n));
        *result = ublas::zero_matrix<prec_t>(n,n);

        #pragma omp parallel for
        for(size_t s = 0; s < n; s++){
            // if this is a terminal state, then just go with zero probabilities
            if(states[s].is_terminal())  continue;

            const Transition&& t = states[s].mean_transition(policy[s],nature[s]);
            const auto& indexes = t.get_indices();
            const auto& probabilities = t.get_probabilities();

            for(size_t j=0; j < t.size(); j++)
                (*result)(indexes[j],s) = probabilities[j];
        }
        return result;
    }

    // ----------------------------------------------
    // Reading and writing files
    // ----------------------------------------------

    /**
    Saves the model to a stream as a simple csv file. States, actions, and outcomes
    are identified by 0-based ids. Columns are separated by commas, and rows by new lines.

    The file is formatted with the following columns:
    idstatefrom, idaction, idoutcome, idstateto, probability, reward

    Exported and imported MDP will be be slightly different. Since action/transitions
    will not be exported if there are no actions for the state. However, when
    there is data for action 1 and action 3, action 2 will be created with no outcomes.

    Note that outcome distributions are not saved.

    \param output Output for the stream
    \param header Whether the header should be written as the
          first line of the file represents the header.
    */
    void to_csv(ostream& output, bool header = true) const{
        //write header is so requested
        if(header){
            output << "idstatefrom," << "idaction," <<
                "idoutcome," << "idstateto," << "probability," << "reward" << endl;
        }

        //idstatefrom
        for(size_t i = 0l; i < this->states.size(); i++){
            const auto& actions = (this->states[i]).get_actions();
            //idaction
            for(size_t j = 0; j < actions.size(); j++){
                const auto& outcomes = actions[j].get_outcomes();
                //idoutcome
                for(size_t k = 0; k < outcomes.size(); k++){
                    const auto& tran = outcomes[k];

                    auto& indices = tran.get_indices();
                    const auto& rewards = tran.get_rewards();
                    const auto& probabilities = tran.get_probabilities();
                    //idstateto
                    for (size_t l = 0; l < tran.size(); l++){
                        output << i << ',' << j << ',' << k << ',' << indices[l] << ','
                                << probabilities[l] << ',' << rewards[l] << endl;
                    }
                }
            }
        }
    }

    /**
    Saves the transition probabilities and rewards to a CSV file
    \param filename Name of the file
    \param header Whether to create a header of the file too
     */
    void to_csv_file(const string& filename, bool header = true) const{
        ofstream ofs(filename, ofstream::out);
        to_csv(ofs,header);
        ofs.close();
    }

    // string representation
    /**
    Returns a brief string representation of the RMDP.
    This method is mostly suitable for analyzing small RMDPs.
    */
    string to_string() const{
        string result;

        for(size_t si : indices(states)){
            const auto& s = get_state(si);
            result += (std::to_string(si));
            result += (" : ");
            result += (std::to_string(s.action_count()));
            result += ("\n");
            for(size_t ai : indices(s)){
                result += ("    ");
                result += (std::to_string(ai));
                result += (" : ");
                const auto& a = s.get_action(ai);
                a.to_string(result);
                result += ("\n");
            }
        }
        return result;
    }

    /**
    Returns a json representation of the RMDP.
    This method is mostly suitable to analyzing small RMDPs.
    */
    string to_json() const{
        string result{"{\"states\" : ["};
        for(auto si : indices(states)){
            const auto& s = states[si];
            result += s.to_json(si);
            result += ",";
        }
        if(!states.empty()) result.pop_back(); // remove last comma
        result += "]}";
        return result;

    }
};

// **********************************************************************
// *********************    TEMPLATE DECLARATIONS    ********************
// **********************************************************************

/**
Regular MDP with discrete actions and one outcome per action

    ActionId = long
    OutcomeId = long

    ActionPolicy = vector<ActionId>
    OutcomePolicy = vector<OutcomeId>

Uncertainty type is ignored in these methods.
*/
typedef GRMDP<RegularState> MDP;

/**
An uncertain MDP with outcomes and weights. See craam::L1RobustState.
*/
typedef GRMDP<WeightedRobustState> RMDP;



}
