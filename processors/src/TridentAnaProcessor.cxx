/** 
 *@file TridentAnaProcessor.cxx
 *@brief Main trident and WAB analysis processor
 *@author MattG, SLAC
 * based off of PF's VertexAnaProcessor
 */

#include "TridentAnaProcessor.h"
#include <iostream>
#include <utility> 
TridentAnaProcessor::TridentAnaProcessor(const std::string& name, Process& process) : Processor(name,process) {

}

//TODO Check this destructor

TridentAnaProcessor::~TridentAnaProcessor(){}

void TridentAnaProcessor::configure(const ParameterSet& parameters) {
    std::cout << "Configuring TridentAnaProcessor" <<std::endl;
    try 
    {
        debug_   = parameters.getInteger("debug");
        anaName_ = parameters.getString("anaName");
        vtxColl_ = parameters.getString("vtxColl");
        trkColl_ = parameters.getString("trkColl");
        trkSelCfg_   = parameters.getString("trkSelectionjson");
        selectionCfg_   = parameters.getString("vtxSelectionjson");
        histoCfg_ = parameters.getString("histoCfg");
        timeOffset_ = parameters.getDouble("CalTimeOffset");
        beamE_  = parameters.getDouble("beamE");
        isData  = parameters.getInteger("isData");

        //region definitions
        regionSelections_ = parameters.getVString("regionDefinitions");
        

    }
    catch (std::runtime_error& error) 
    {
        std::cout<<error.what()<<std::endl;
    }
}

void TridentAnaProcessor::initialize(TTree* tree) {
    tree_ = tree;
    _ah =  std::make_shared<AnaHelpers>();
    
    trkSelector  = std::make_shared<BaseSelector>("trkSelection",trkSelCfg_);
    trkSelector->setDebug(debug_);
    trkSelector->LoadSelection();
        
    vtxSelector  = std::make_shared<BaseSelector>("vtxSelection",selectionCfg_);
    vtxSelector->setDebug(debug_);
    vtxSelector->LoadSelection();
    
    _vtx_histos = std::make_shared<TrackHistos>("vtxSelection");
    _vtx_histos->loadHistoConfig(histoCfg_);
    _vtx_histos->DefineHistos();
    
    
     // //For each region initialize plots
     
     for (unsigned int i_reg = 0; i_reg < regionSelections_.size(); i_reg++) {
       std::string regname = AnaHelpers::getFileName(regionSelections_[i_reg],false);
       std::cout<<"Setting up region:: " << regname <<std::endl;   
       _reg_vtx_selectors[regname] = std::make_shared<BaseSelector>(regname, regionSelections_[i_reg]);
       _reg_vtx_selectors[regname]->setDebug(debug_);
       _reg_vtx_selectors[regname]->LoadSelection();
       
       _reg_vtx_histos[regname] = std::make_shared<TrackHistos>(regname);
       _reg_vtx_histos[regname]->loadHistoConfig(histoCfg_);
       _reg_vtx_histos[regname]->DefineHistos();
       
       _reg_tuples[regname] = std::make_shared<FlatTupleMaker>(regname+"_tree");
       _reg_tuples[regname]->addVariable("unc_vtx_mass");
       _reg_tuples[regname]->addVariable("unc_vtx_z");
       
       _regions.push_back(regname);
     }
     
     
     //init Reading Tree
     tree_->SetBranchAddress(vtxColl_.c_str(), &vtxs_ , &bvtxs_);
     tree_->SetBranchAddress(fspartColl_.c_str(), &fspart_ , &bfspart_);
     tree_->SetBranchAddress("EventHeader",&evth_ , &bevth_);
    
     //If track collection name is empty take the tracks from the particles. TODO:: change this
     if (!trkColl_.empty())
       tree_->SetBranchAddress(trkColl_.c_str(),&trks_, &btrks_);
}

bool TridentAnaProcessor::process(IEvent* ievent) { 
    
    HpsEvent* hps_evt = (HpsEvent*) ievent;
    double weight = 1.;

    //Store processed number of events
    std::vector<Vertex*> selected_vtxs;
    
    std::vector<Particle*> goodElectrons;
    std::vector<Particle*> goodPositrons;
    std::vector<Particle*> goodPhotons;
    std::vector<Vertex*> goodV0s;

    std::vector<Particle*> noDups;

    // remove similar tracks == more than 1 shared hit; choose "best" track
    for (int i_part;i_part<fspart_->size();i_part++){
      Particle* part1=fspart_->at(i_part);
      if(_ah->IsBestTrack(part1,*trks_))
	noDups.push_back(part1);      
    }
    if(debug_ && fspart_->size()!=noDups.size())
      std::cout<<"with dups = "<<fspart_->size()<<";  no dups = "<<noDups.size()<<std::endl;
    

    for(int i_part; i_part<noDups.size();i_part++){
      Particle* part=noDups.at(i_part);
      int ch=part->getCharge();
      if(ch==0){	
	goodPhotons.push_back(part); //all good photons are good
	continue;
      }

      if(ch==-1){
	goodElectrons.push_back(part);
      }else{
	goodPositrons.push_back(part);
      }	
      
    }
    if(debug_)
      std::cout<<"Electrons = "<<goodElectrons.size()<<"; Positrons = "<<goodPositrons.size()<<"; Photons = "<<goodPhotons.size()<<std::endl;

    // make WAB (gamma e-) and trident (e+e-) candidates
    std::vector <std::pair<Particle*,Particle*>> wabPairs;
    std::vector <std::pair<Particle*,Particle*>> triPairs;

    

    
    for ( int i_vtx = 0; i_vtx <  vtxs_->size(); i_vtx++ ) {
        
        vtxSelector->getCutFlowHisto()->Fill(0.,weight);
        
        Vertex* vtx = vtxs_->at(i_vtx);
        Particle* ele = nullptr;
        Track* ele_trk = nullptr;
        Particle* pos = nullptr;
        Track* pos_trk = nullptr;

        //Trigger requirement - *really hate* having to do it here for each vertex.
            
        if (isData) {
            if (!vtxSelector->passCutEq("Pair1_eq",(int)evth_->isPair1Trigger(),weight))
                break;
        }
                
        bool foundParts = _ah->GetParticlesFromVtx(vtx,ele,pos);
        if (!foundParts) {
            //std::cout<<"TridentAnaProcessor::WARNING::Found vtx without ele/pos. Skip."
            continue;
        }
        
        if (!trkColl_.empty()) {
            bool foundTracks = _ah->MatchToGBLTracks((ele->getTrack()).getID(),(pos->getTrack()).getID(),
                                                     ele_trk, pos_trk, *trks_);
            if (!foundTracks) {
                //std::cout<<"TridentAnaProcessor::ERROR couldn't find ele/pos in the GBLTracks collection"<<std::endl;
                continue;  
            }
        }
        else {
            ele_trk = (Track*)ele->getTrack().Clone();
            pos_trk = (Track*)pos->getTrack().Clone();
        }
        
        //Add the momenta to the tracks
        ele_trk->setMomentum(ele->getMomentum()[0],ele->getMomentum()[1],ele->getMomentum()[2]);
        pos_trk->setMomentum(pos->getMomentum()[0],pos->getMomentum()[1],pos->getMomentum()[2]);
                
        
        //Tracks in opposite volumes - useless
        //if (!vtxSelector->passCutLt("eleposTanLambaProd_lt",ele_trk->getTanLambda() * pos_trk->getTanLambda(),weight)) 
        //  continue;
        
        //Ele Track-cluster match
        if (!vtxSelector->passCutLt("eleTrkCluMatch_lt",ele->getGoodnessOfPID(),weight))
            continue;

        //Pos Track-cluster match
        if (!vtxSelector->passCutLt("posTrkCluMatch_lt",pos->getGoodnessOfPID(),weight))
            continue;

        double corr_eleClusterTime = ele->getCluster().getTime() - timeOffset_;
        double corr_posClusterTime = pos->getCluster().getTime() - timeOffset_;
                
        //Ele Pos Cluster Tme Difference
        if (!vtxSelector->passCutLt("eleposCluTimeDiff_lt",fabs(corr_eleClusterTime - corr_posClusterTime),weight))
            continue;
        
        //Ele Track-Cluster Time Difference
        if (!vtxSelector->passCutLt("eleTrkCluTimeDiff_lt",fabs(ele_trk->getTrackTime() - corr_eleClusterTime),weight))
            continue;
        
        //Pos Track-Cluster Time Difference
        if (!vtxSelector->passCutLt("posTrkCluTimeDiff_lt",fabs(pos_trk->getTrackTime() - corr_posClusterTime),weight))
            continue;
        
        TVector3 ele_mom;
        ele_mom.SetX(ele->getMomentum()[0]);
        ele_mom.SetY(ele->getMomentum()[1]);
        ele_mom.SetZ(ele->getMomentum()[2]);

        TVector3 pos_mom;
        pos_mom.SetX(pos->getMomentum()[0]);
        pos_mom.SetY(pos->getMomentum()[1]);
        pos_mom.SetZ(pos->getMomentum()[2]);
        
        
        //Beam Electron cut
        if (!vtxSelector->passCutLt("eleMom_lt",ele_mom.Mag(),weight))
            continue;

        //Ele Track Quality
        if (!vtxSelector->passCutLt("eleTrkChi2_lt",ele_trk->getChi2Ndf(),weight))
            continue;
        
        //Pos Track Quality
        if (!vtxSelector->passCutLt("posTrkChi2_lt",pos_trk->getChi2Ndf(),weight))
            continue;

        //Vertex Quality
        if (!vtxSelector->passCutLt("chi2unc_lt",vtx->getChi2(),weight))
            continue;
        

        //Ele min momentum cut
        if (!vtxSelector->passCutGt("eleMom_gt",ele_mom.Mag(),weight))
            continue;

        //Pos min momentum cut
        if (!vtxSelector->passCutGt("posMom_gt",pos_mom.Mag(),weight))
            continue;

        //Max vtx momentum
        
        if (!vtxSelector->passCutLt("maxVtxMom_lt",(ele_mom+pos_mom).Mag(),weight))
            continue;
        
        _vtx_histos->Fill1DVertex(vtx,
                                  ele,
                                  pos,
                                  ele_trk,
                                  pos_trk,
                                  weight);
        
        _vtx_histos->Fill2DHistograms(vtx,weight);
        _vtx_histos->Fill2DTrack(ele_trk,weight,"ele_");
        _vtx_histos->Fill2DTrack(pos_trk,weight,"pos_");
        
        selected_vtxs.push_back(vtx);       
        vtxSelector->clearSelector();
    }
    
    _vtx_histos->Fill1DHisto("n_vertices_h",selected_vtxs.size()); 
    if (trks_)
        _vtx_histos->Fill1DHisto("n_tracks_h",trks_->size()); 
    
    
    //Make Plots for each region: loop on each region. Check if the region has the cut and apply it

    for ( auto vtx : selected_vtxs) {
        
        for (auto region : _regions ) {
            
            //No cuts.
            _reg_vtx_selectors[region]->getCutFlowHisto()->Fill(0.,weight);
            
            
            Particle* ele = nullptr;
            Particle* pos = nullptr;
            
            _ah->GetParticlesFromVtx(vtx,ele,pos);
            
            //Chi2
            if (!_reg_vtx_selectors[region]->passCutLt("chi2unc_lt",vtx->getChi2(),weight))
                continue;
            
            double ele_E = ele->getEnergy();
            double pos_E = pos->getEnergy();


            //Compute analysis variables here.
            
            Track ele_trk = ele->getTrack();
            Track pos_trk = pos->getTrack();
            
            //Get the shared info - TODO change and improve
            
            Track* ele_trk_gbl = nullptr;
            Track* pos_trk_gbl = nullptr;
            
            if (!trkColl_.empty()) {
                bool foundTracks = _ah->MatchToGBLTracks(ele_trk.getID(),pos_trk.getID(),
                                                         ele_trk_gbl, pos_trk_gbl, *trks_);
                
                if (!foundTracks) {
                    //std::cout<<"TridentAnaProcessor::ERROR couldn't find ele/pos in the GBLTracks collection"<<std::endl;
                    continue;  
                }
            }
            else {
                
                ele_trk_gbl = (Track*) ele_trk.Clone();
                pos_trk_gbl = (Track*) pos_trk.Clone();
            }
            
            //Add the momenta to the tracks
            ele_trk_gbl->setMomentum(ele->getMomentum()[0],ele->getMomentum()[1],ele->getMomentum()[2]);
            pos_trk_gbl->setMomentum(pos->getMomentum()[0],pos->getMomentum()[1],pos->getMomentum()[2]);


	    TVector3 ele_mom;
	    ele_mom.SetX(ele->getMomentum()[0]);
	    ele_mom.SetY(ele->getMomentum()[1]);
	    ele_mom.SetZ(ele->getMomentum()[2]);

	    TVector3 pos_mom;
	    pos_mom.SetX(pos->getMomentum()[0]);
	    pos_mom.SetY(pos->getMomentum()[1]);
	    pos_mom.SetZ(pos->getMomentum()[2]);

                       
            bool foundL1ele = false;
            bool foundL2ele = false;
            _ah->InnermostLayerCheck(ele_trk_gbl, foundL1ele, foundL2ele);   
            
            bool foundL1pos = false;
            bool foundL2pos = false;
            _ah->InnermostLayerCheck(pos_trk_gbl, foundL1pos, foundL2pos);  
            
	    int layerCombo=-1;
	    if (foundL1pos&&foundL1ele) //L1L1
	      layerCombo=1;
	    if(!foundL1pos&&foundL2pos&&foundL1ele)//L2L1
	      layerCombo=2;
	    if(foundL1pos&&!foundL1ele&&foundL2ele)//L1L2
	      layerCombo=3;
	    if(!foundL1pos&&foundL2pos&&!foundL1ele&&foundL2ele)//L2L2
	      layerCombo=4;
	    
	    //	    std::cout<<"checking layer cut   "<<layerCombo<<std::endl;            
            if (!_reg_vtx_selectors[region]->passCutEq("LayerRequirement",layerCombo,weight))
	      continue;
	    //std::cout<<"passed layer cut"<<std::endl;
            
            //ESum low cut 
            if (!_reg_vtx_selectors[region]->passCutLt("pSum_lt",(ele_mom+pos_mom).Mag()/beamE_,weight))
                continue;
             
            //ESum hight cut
            if (!_reg_vtx_selectors[region]->passCutGt("pSum_gt",(ele_mom+pos_mom).Mag()/beamE_,weight))
                continue;
            
            
            //No shared hits requirement
	    /*	             if (!_reg_vtx_selectors[region]->passCutEq("ele_sharedL0_eq",(int)ele_trk_gbl->getSharedLy0(),weight))
                continue;
            if (!_reg_vtx_selectors[region]->passCutEq("pos_sharedL0_eq",(int)pos_trk_gbl->getSharedLy0(),weight))
                continue;
            if (!_reg_vtx_selectors[region]->passCutEq("ele_sharedL1_eq",(int)ele_trk_gbl->getSharedLy1(),weight))
                continue;
            if (!_reg_vtx_selectors[region]->passCutEq("pos_sharedL1_eq",(int)pos_trk_gbl->getSharedLy1(),weight))
                continue;
	    */
	    

            //N selected vertices - this is quite a silly cut to make at the end. But okay. that's how we decided atm.
            if (!_reg_vtx_selectors[region]->passCutEq("nVtxs_eq",selected_vtxs.size(),weight))	      
	      continue;
            _reg_vtx_histos[region]->Fill2DHistograms(vtx,weight);
            _reg_vtx_histos[region]->Fill1DVertex(vtx,
                                                  ele,
                                                  pos,
                                                  ele_trk_gbl,
                                                  pos_trk_gbl,
                                                  weight);

            _reg_vtx_histos[region]->Fill2DTrack(ele_trk_gbl,weight,"ele_");
            _reg_vtx_histos[region]->Fill2DTrack(pos_trk_gbl,weight,"pos_");
            

            
            if (trks_)
                _reg_vtx_histos[region]->Fill1DHisto("n_tracks_h",trks_->size(),weight);
            _reg_vtx_histos[region]->Fill1DHisto("n_vertices_h",selected_vtxs.size(),weight);
            
            
            //Just for the selected vertex
            _reg_tuples[region]->setVariableValue("unc_vtx_mass", vtx->getInvMass());
            
            //TODO put this in the Vertex!
            TVector3 vtxPosSvt;
            vtxPosSvt.SetX(vtx->getX());
            vtxPosSvt.SetY(vtx->getY());
            vtxPosSvt.SetZ(vtx->getZ());
            vtxPosSvt.RotateY(-0.0305);
            
            _reg_tuples[region]->setVariableValue("unc_vtx_z"   , vtxPosSvt.Z());
            _reg_tuples[region]->fill();
        }// regions
    } // preselected vertices
    
    
    
    return true;
}

void TridentAnaProcessor::finalize() {
    
    //TODO clean this up a little.
    outF_->cd();
    _vtx_histos->saveHistos(outF_,_vtx_histos->getName());
    outF_->cd(_vtx_histos->getName().c_str());
    vtxSelector->getCutFlowHisto()->Write();
        
    
    for (reg_it it = _reg_vtx_histos.begin(); it!=_reg_vtx_histos.end(); ++it) {
        (it->second)->saveHistos(outF_,it->first);
        outF_->cd((it->first).c_str());
        _reg_vtx_selectors[it->first]->getCutFlowHisto()->Write();
        //Save tuples
        _reg_tuples[it->first]->writeTree();
    }
    
    outF_->Close();
        
}

DECLARE_PROCESSOR(TridentAnaProcessor);
