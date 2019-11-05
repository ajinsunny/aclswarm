/**
 * @file auctioneer.cpp
 * @brief Distributed task assignment for aclswarm via CBAA
 * @author Parker Lusk <parkerclusk@gmail.com>
 * @date 26 Oct 2019
 */

#include "aclswarm/auctioneer.h"

namespace acl {
namespace aclswarm {

Auctioneer::Auctioneer(vehidx_t vehid, uint8_t n)
: n_(n), vehid_(vehid), biditer_(-1), bid_(new Bid)
{
  P_.setIdentity(n_);
  Pt_.setIdentity(n_);
}

// ----------------------------------------------------------------------------

void Auctioneer::setNewAssignmentHandler(
                                std::function<void(const AssignmentPerm&)> f)
{
  fn_assignment_ = f;
}

// ----------------------------------------------------------------------------

void Auctioneer::setSendBidHandler(
          std::function<void(uint32_t, const Auctioneer::BidConstPtr&)> f)
{
  fn_sendbid_ = f;
}

// ----------------------------------------------------------------------------

void Auctioneer::setFormation(const PtsMat& p, const AdjMat& adjmat,
                                                        bool resetAssignment)
{
  assert(n_ == p.rows());
  assert(n_ == adjmat.rows());

  p_ = p;
  adjmat_ = adjmat;

  constexpr uint32_t diameter = 2; // hardcoded for now...
  cbaa_max_iter_ = n_ * diameter;

  reset();

  if (resetAssignment) {
    P_.setIdentity(n_);
    Pt_.setIdentity(n_);
  }
}

// ----------------------------------------------------------------------------

void Auctioneer::start(const PtsMat& q)
{
  std::lock_guard<std::mutex> lock(mtx_);

  // Assumption: my internal state has already been (re)initialized

  // store the current state of vehicles in the swarm to be used throughout
  q_ = q;

  //
  // Alignment
  //

  // align current swarm positions to desired formation (using neighbors only)
  paligned_ = alignFormation(q_, adjmat_, p_);

  //
  // Assignment (kick off with an initial bid)
  //

  // Using only knowledgde of my current state and what I think the aligned
  // formation is, make an initial bid for the formation point I am closest to.
  selectTaskAssignment();

  // indicates that the auction has started
  biditer_ = 0;

  // send my bid to my neighbors
  notifySendBid();

  // let any threads waiting on the start bid iteration know we are done
  cv_.notify_all();
}

// ----------------------------------------------------------------------------

void Auctioneer::receiveBid(uint32_t iter, const Bid& bid, vehidx_t vehid)
{
  std::unique_lock<std::mutex> lock(mtx_);
  cv_.wait(lock, [this](){ return biditer_ >= 0; });

  // keep track of all bids across bid iterations
  bids_[iter].insert({vehid, bid});

  // once my neighbors' bids are in, tally them up and decide who the winner is
  if (bidIterComplete()) {

    //
    // Update CBAA
    //

    // update my local understanding of who deserves which task based on the
    // highest bidder of each task, within my neighborhood.
    bool was_outbid = updateTaskAssignment();

    // If I was outbid, I will need to select a new task.
    if (was_outbid) selectTaskAssignment();

    // start the next iteration
    ++biditer_;

    //
    // Determine convergence or continue bidding
    //

    if (hasReachedConsensus()) {
      // Extract the best assignment from my local understanding,
      // which has reached consensus since the auction is complete.

      // note: we are making implicit type casts here
      std::vector<vehidx_t> tmp(bid_->who.begin(), bid_->who.end());

      // n.b., 'who' maps task --> vehid, which is P^T
      const auto newPt = AssignmentPerm(Eigen::Map<AssignmentVec>(tmp.data(), tmp.size()));
      const auto newP = newPt.transpose();

      // log the assignment for debugging
      logAssignment(q_, adjmat_, p_, paligned_, P_, newP);

      // determine if this assignment is better than the previous one
      if (shouldUseAssignment(newP)) {
        // set the assignment
        P_ = newP;
        Pt_ = newPt;

        // let the caller know a new assignment is ready
        notifyNewAssignment();
      }

      // get ready for next auction, makes auctioneer idle
      reset();
    } else {
      // send latest bid to my neighbors
      notifySendBid();
    }
  }
}

// ----------------------------------------------------------------------------
// Private Methods
// ----------------------------------------------------------------------------

void Auctioneer::notifySendBid()
{
  // let the caller know
  fn_sendbid_(biditer_, bid_);
}

// ----------------------------------------------------------------------------

void Auctioneer::notifyNewAssignment()
{
  // let the caller know
  fn_assignment_(P_);
}

// ----------------------------------------------------------------------------

bool Auctioneer::shouldUseAssignment(const AssignmentPerm& newP) const
{
  // don't bother if the assignment is the same
  if (P_.indices().isApprox(newP.indices())) return false;

  {
    //
    // Select Local Information (i.e., use only nbrs for alignment)
    //

    // work in "formation space" since we are using the adjmat to check nbhrs
    const vehidx_t i = P_.indices()(vehid_);

    // keep track this vehicle's neighbors ("formation space")---include myself
    std::vector<vehidx_t> nbrpts;
    for (size_t j=0; j<n_; ++j) if (adjmat_(i, j) || i==j) nbrpts.push_back(j);

    // extract local nbrhd information for this vehicle to use in alignment
    PtsMat pnbrs = PtsMat::Zero(nbrpts.size(), 3);
    PtsMat qnbrs = PtsMat::Zero(nbrpts.size(), 3);
    for (size_t k=0; k<nbrpts.size(); ++k) {
      // correspondence btwn nbr formpts and nbr vehicles
      pnbrs.row(k) = p_.row(nbrpts[k]);
      qnbrs.row(k) = q_.row(Pt_.indices()(nbrpts[k]));
    }

    //
    // Determine if the formation is a colinear, flat, or 3d
    //

    // be very stringent on what we consider a nonzero singular value (for noise)
    constexpr double RANK_TH = 0.05;
    Eigen::JacobiSVD<PtsMat> svdQ(qnbrs.rowwise() - qnbrs.colwise().mean());
    Eigen::JacobiSVD<PtsMat> svdP(pnbrs.rowwise() - pnbrs.colwise().mean());
    svdQ.setThreshold(RANK_TH);
    svdP.setThreshold(RANK_TH);

    // Only perform 3D Umeyama if there is structure in both swarm and formation
    int d = (svdQ.rank() == 3 && svdP.rank() == 3) ? 3 : 2;

    //
    // Point cloud alignment (maps p onto q)
    //

    // We need our point clouds to be stored in 3xN matrices
    const Eigen::Matrix<double, 3, Eigen::Dynamic> pp = pnbrs.transpose();
    const Eigen::Matrix<double, 3, Eigen::Dynamic> qq = qnbrs.transpose();

    const auto T = Eigen::umeyama(pp.topRows(d), qq.topRows(d), false);

    // for the extracted transformation that maps p onto q
    Eigen::Matrix3d R = Eigen::Matrix3d::Identity();
    Eigen::Vector3d t = Eigen::Vector3d::Zero();

    // Make sure to embed back in 3D if necessary
    if (d == 2) {
      R.block<2,2>(0,0) = T.block<2,2>(0,0);
      t.head(2) = T.block<2,1>(0,2);
    } else {
      R.block<3,3>(0,0) = T.block<3,3>(0,0);
      t = T.block<3,1>(0,3);
    }

    // make sure to send back as an Nx3 PtsMat
    PtsMat aligned = ((R * pnbrs.transpose()).colwise() + t).transpose();

    std::cout << "current P: " << (qnbrs - aligned).colwise().squaredNorm().sum() << std::endl;
  }

  {
    //
    // Select Local Information (i.e., use only nbrs for alignment)
    //

    // work in "formation space" since we are using the adjmat to check nbhrs
    const vehidx_t i = newP.indices()(vehid_);

    const AssignmentPerm newPt = newP.transpose();

    // keep track this vehicle's neighbors ("formation space")---include myself
    std::vector<vehidx_t> nbrpts;
    for (size_t j=0; j<n_; ++j) if (adjmat_(i, j) || i==j) nbrpts.push_back(j);

    // extract local nbrhd information for this vehicle to use in alignment
    PtsMat pnbrs = PtsMat::Zero(nbrpts.size(), 3);
    PtsMat qnbrs = PtsMat::Zero(nbrpts.size(), 3);
    for (size_t k=0; k<nbrpts.size(); ++k) {
      // correspondence btwn nbr formpts and nbr vehicles
      pnbrs.row(k) = p_.row(nbrpts[k]);
      qnbrs.row(k) = q_.row(newPt.indices()(nbrpts[k]));
    }

    //
    // Determine if the formation is a colinear, flat, or 3d
    //

    // be very stringent on what we consider a nonzero singular value (for noise)
    constexpr double RANK_TH = 0.05;
    Eigen::JacobiSVD<PtsMat> svdQ(qnbrs.rowwise() - qnbrs.colwise().mean());
    Eigen::JacobiSVD<PtsMat> svdP(pnbrs.rowwise() - pnbrs.colwise().mean());
    svdQ.setThreshold(RANK_TH);
    svdP.setThreshold(RANK_TH);

    // Only perform 3D Umeyama if there is structure in both swarm and formation
    int d = (svdQ.rank() == 3 && svdP.rank() == 3) ? 3 : 2;

    //
    // Point cloud alignment (maps p onto q)
    //

    // We need our point clouds to be stored in 3xN matrices
    const Eigen::Matrix<double, 3, Eigen::Dynamic> pp = pnbrs.transpose();
    const Eigen::Matrix<double, 3, Eigen::Dynamic> qq = qnbrs.transpose();

    const auto T = Eigen::umeyama(pp.topRows(d), qq.topRows(d), false);

    // for the extracted transformation that maps p onto q
    Eigen::Matrix3d R = Eigen::Matrix3d::Identity();
    Eigen::Vector3d t = Eigen::Vector3d::Zero();

    // Make sure to embed back in 3D if necessary
    if (d == 2) {
      R.block<2,2>(0,0) = T.block<2,2>(0,0);
      t.head(2) = T.block<2,1>(0,2);
    } else {
      R.block<3,3>(0,0) = T.block<3,3>(0,0);
      t = T.block<3,1>(0,3);
    }

    // make sure to send back as an Nx3 PtsMat
    PtsMat aligned = ((R * pnbrs.transpose()).colwise() + t).transpose();

    std::cout << "new P: " << (qnbrs - aligned).colwise().squaredNorm().sum() << std::endl;
  }


  return true;
}

// ----------------------------------------------------------------------------

PtsMat Auctioneer::alignFormation(const PtsMat& q,
                                  const AdjMat& adjmat, const PtsMat& p) const
{
  // Find (R,t) that minimizes ||q - (Rp + t)||^2

  //
  // Select Local Information (i.e., use only nbrs for alignment)
  //

  // work in "formation space" since we are using the adjmat to check nbhrs
  const vehidx_t i = P_.indices()(vehid_);

  // keep track this vehicle's neighbors ("formation space")---include myself
  std::vector<vehidx_t> nbrpts;
  for (size_t j=0; j<n_; ++j) if (adjmat(i, j) || i==j) nbrpts.push_back(j);

  // extract local nbrhd information for this vehicle to use in alignment
  PtsMat pnbrs = PtsMat::Zero(nbrpts.size(), 3);
  PtsMat qnbrs = PtsMat::Zero(nbrpts.size(), 3);
  for (size_t k=0; k<nbrpts.size(); ++k) {
    // correspondence btwn nbr formpts and nbr vehicles
    pnbrs.row(k) = p.row(nbrpts[k]);
    qnbrs.row(k) = q.row(Pt_.indices()(nbrpts[k]));
  }

  //
  // Determine if the formation is a colinear, flat, or 3d
  //

  // be very stringent on what we consider a nonzero singular value (for noise)
  constexpr double RANK_TH = 0.05;
  Eigen::JacobiSVD<PtsMat> svdQ(qnbrs.rowwise() - qnbrs.colwise().mean());
  Eigen::JacobiSVD<PtsMat> svdP(pnbrs.rowwise() - pnbrs.colwise().mean());
  svdQ.setThreshold(RANK_TH);
  svdP.setThreshold(RANK_TH);

  // Only perform 3D Umeyama if there is structure in both swarm and formation
  int d = (svdQ.rank() == 3 && svdP.rank() == 3) ? 3 : 2;

  //
  // Point cloud alignment (maps p onto q)
  //

  // We need our point clouds to be stored in 3xN matrices
  const Eigen::Matrix<double, 3, Eigen::Dynamic> pp = pnbrs.transpose();
  const Eigen::Matrix<double, 3, Eigen::Dynamic> qq = qnbrs.transpose();

  const auto T = Eigen::umeyama(pp.topRows(d), qq.topRows(d), false);

  // for the extracted transformation that maps p onto q
  Eigen::Matrix3d R = Eigen::Matrix3d::Identity();
  Eigen::Vector3d t = Eigen::Vector3d::Zero();

  // Make sure to embed back in 3D if necessary
  if (d == 2) {
    R.block<2,2>(0,0) = T.block<2,2>(0,0);
    t.head(2) = T.block<2,1>(0,2);
  } else {
    R.block<3,3>(0,0) = T.block<3,3>(0,0);
    t = T.block<3,1>(0,3);
  }

  // make sure to send back as an Nx3 PtsMat
  PtsMat aligned = ((R * p.transpose()).colwise() + t).transpose();
  return aligned;
}

// ----------------------------------------------------------------------------

bool Auctioneer::bidIterComplete() const
{
  // for convenience: my neighbors' bids from this bid iteration
  const auto& bids = bids_[biditer_];

  // work in "formation space" since we are using the adjmat to check nbhrs
  const vehidx_t i = P_.indices()(vehid_);

  for (size_t j=0; j<n_; ++j) {
    if (adjmat_(i, j)) {
      // map back to "vehicle space" since that's how our bids are keyed
      vehidx_t nbhr = Pt_.indices()(j);

      // CBAA iteration is not complete if I am missing any of my nbhrs' bids
      if (bids.find(nbhr) == bids.end()) return false;
    }
  }

  return true;
}

// ----------------------------------------------------------------------------

bool Auctioneer::hasReachedConsensus() const
{
  return biditer_ >= cbaa_max_iter_;
}

// ----------------------------------------------------------------------------

void Auctioneer::reset()
{
  // indicates that there is no auction being performed
  biditer_ = -1;

  // initialize my bid
  bid_->price.clear();
  std::fill_n(std::back_inserter(bid_->price), n_, 0.0);
  bid_->who.clear();
  std::fill_n(std::back_inserter(bid_->who), n_, -1);

  // Create a table to hold my neighbor's bids for each CBAA iteration.
  bids_.clear();
  bids_.resize(cbaa_max_iter_);
}

// ----------------------------------------------------------------------------

bool Auctioneer::updateTaskAssignment()
{
  // Given all of the bids from my neighbors (and me), which agent is most
  // deserving of each of the formation points (tasks)? In other words,
  // At this point in the current auction, who currently has the highest bids?
  // n.b., a nbhr might have *info* about who has the highest bid for a given
  // formpt, but it may not be that nbhr---it's just in their local info

  // for convenience: my neighbors' bids from this bid iteration
  auto& bids = bids_[biditer_];
  bool was_outbid = false;

  // add myself so that my local information is considered
  bids.insert({vehid_, *bid_});

  // loop through each task and decide on the winner
  for (size_t j=0; j<n_; ++j) {

    //
    // Which of my nbhrs (or me) bid the most for task / formpt j?
    //

    // arbitrarily assume that the first nhbr in the list has the highest bid
    auto maxit = bids.cbegin();

    // but then loop through each nbhr (and me) and decide who bid the most
    for (auto it = bids.cbegin(); it!=bids.cend(); it++) {
      if (it->second.price[j] > maxit->second.price[j]) maxit = it;
    }

    //
    // Update my local understanding of who has bid the most for each task
    //

    // check if I was outbid by someone else
    if (bid_->who[j] == vehid_ && maxit->second.who[j] != vehid_) was_outbid = true;

    // who should be assigned task j?
    bid_->who[j] = maxit->second.who[j];

    // how much is this winning agent willing to bid on this task?
    bid_->price[j] = maxit->second.price[j];
  }

  // did someone outbid me for my desired formation point / task?
  return was_outbid;
}

// ----------------------------------------------------------------------------

void Auctioneer::selectTaskAssignment()
{
  // Determine the highest price this agent is willing to pay to be assigned
  // a specific task / formpt.
  float max = 0;
  size_t task = 0;
  bool was_assigned = false;
  for (size_t j=0; j<n_; ++j) {
    // n.b., within the same auction, this list of prices will be the same
    const float price = getPrice(q_.row(vehid_), paligned_.row(j));

    // In addition to finding the task that I am most interested in,
    // only bid on a task if I think I will win (highest bidder of my nbhrs)
    if (price > max && price > bid_->price[j]) {
      max = price;
      task = j;
      was_assigned = true;
    }
  }

  // update my local information to reflect my bid
  if (was_assigned) { // TODO: will this always be true?
    bid_->price[task] = max;
    bid_->who[task] = vehid_;
  }
}

// ----------------------------------------------------------------------------

float Auctioneer::getPrice(const Eigen::Vector3d& p1, const Eigen::Vector3d& p2)
{
  return 1.0 / ((p1 - p2).norm() + 1e-8);
}

// ----------------------------------------------------------------------------

void Auctioneer::logAssignment(const PtsMat& q, const AdjMat& adjmat,
                        const PtsMat& p, const PtsMat& aligned,
                        const AssignmentPerm& lastP, const AssignmentPerm& P)
{
  // open a binary file
  std::ofstream bin("alignment_" + std::to_string(vehid_) + ".bin",
                    std::ios::binary);

  bin.write(reinterpret_cast<const char *>(&n_), sizeof n_);
  bin.write(reinterpret_cast<const char *>(q.data()), sizeof(q.data()[0])*q.size());
  bin.write(reinterpret_cast<const char *>(adjmat.data()), sizeof(adjmat.data()[0])*adjmat.size());
  bin.write(reinterpret_cast<const char *>(lastP.indices().data()), sizeof(lastP.indices().data()[0])*lastP.indices().size());
  bin.write(reinterpret_cast<const char *>(p.data()), sizeof(p.data()[0])*p.size());
  bin.write(reinterpret_cast<const char *>(aligned.data()), sizeof(aligned.data()[0])*aligned.size());
  bin.write(reinterpret_cast<const char *>(P.indices().data()), sizeof(P.indices().data()[0])*P.indices().size());
  bin.close();
}

} // ns aclswarm
} // ns acl