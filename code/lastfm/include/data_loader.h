#ifndef DATA_LOADER_H
#define DATA_LOADER_H

#include "graph_data.h"
#include <vector>
#include <iostream>
#include <fstream>
#include <sstream>
#include <deque>
#include <set>
#include "sparse_matrix.h"
#include "dense_matrix.h"

class IDataLoader
{
public:
   
    IDataLoader(size_t _num_events, size_t _batch_size) : num_events(_num_events), batch_size(_batch_size)
    {
        event_sequences.clear();
        time_sequences.clear();
        cursors.resize(batch_size);
        index_pool.clear();
        num_samples = 0;
        initialized = false;
    }
    
    inline void InsertSequence(int* event_seq, Dtype* time_seq, int seq_len)
    {
        num_samples += seq_len - 1;
        InsertSequence(event_seq, event_sequences, seq_len);
        InsertSequence(time_seq, time_sequences, seq_len);   
    }
    
    virtual void StartNewEpoch()
    {
        initialized = true;
        if (index_pool.size() != event_sequences.size())
        {
            index_pool.clear();
            assert(event_sequences.size() == time_sequences.size()); 
            for (unsigned i = 0; i < event_sequences.size(); ++i)
            {
                index_pool.push_back(i);                     
            }
        }
        for (unsigned i = 0; i < batch_size; ++i)
        {
            cursors[i].first = index_pool.front();
            cursors[i].second = 0;
            index_pool.pop_front();
        }
    }
    
    size_t num_samples, num_events, batch_size; 
    
protected:
    template<typename data_type>
    inline void InsertSequence(data_type* seq, std::vector< std::vector<data_type> >& sequences, int seq_len)
    {
        std::vector<data_type> cur_seq;
        cur_seq.clear();
        for (int i = 0; i < seq_len; ++i)
            cur_seq.push_back(seq[i]);
        sequences.push_back(cur_seq);
    }   
    
    template<MatMode mode>
    void ReloadSlot(GraphData<mode, Dtype>* g_last_hidden, unsigned batch_idx)
    {
        DenseMat<mode, Dtype> row_submat;
        g_last_hidden->node_states->DenseDerived().GetRows(batch_idx, 1, row_submat);
        row_submat.Fill(0);
        
        index_pool.push_back(cursors[batch_idx].first); 
        cursors[batch_idx].first = index_pool.front();
        cursors[batch_idx].second = 0;
        index_pool.pop_front();
    }
    
    template<MatMode mode>
    void LoadEvent(GraphData<mode, Dtype>* g_feat, GraphData<mode, Dtype>* g_label, unsigned cur_batch_size, unsigned step)
    {
        auto& feat = g_feat->node_states->SparseDerived();
        auto& label = g_label->node_states->SparseDerived();
        
        event_feat_cpu.Resize(cur_batch_size, num_events);
        event_feat_cpu.ResizeSp(cur_batch_size, cur_batch_size + 1); 
        
        event_label_cpu.Resize(cur_batch_size, num_events);
        event_label_cpu.ResizeSp(cur_batch_size, cur_batch_size + 1);
        
        for (unsigned i = 0; i < cur_batch_size; ++i)
        {
            event_feat_cpu.data->ptr[i] = i;
            event_feat_cpu.data->col_idx[i] = event_sequences[cursors[i].first][cursors[i].second + step]; 
            event_feat_cpu.data->val[i] = 1;
            
            event_label_cpu.data->ptr[i] = i;
            event_label_cpu.data->col_idx[i] = event_sequences[cursors[i].first][cursors[i].second + step + 1];
            event_label_cpu.data->val[i] = 1;                        
        }
        event_feat_cpu.data->ptr[cur_batch_size] = cur_batch_size;
        event_label_cpu.data->ptr[cur_batch_size] = cur_batch_size;
        
        feat.CopyFrom(event_feat_cpu);
        label.CopyFrom(event_label_cpu);
    } 
    
    template<MatMode mode>
    void LoadTime(GraphData<mode, Dtype>* g_feat, GraphData<mode, Dtype>* g_label, unsigned cur_batch_size, unsigned step)
    {
        auto& feat = g_feat->node_states->DenseDerived();
        auto& label = g_label->node_states->DenseDerived();
        
        time_feat_cpu.Resize(cur_batch_size, 1);
        time_label_cpu.Resize(cur_batch_size, 1);
        
        for (unsigned i = 0; i < cur_batch_size; ++i)
        {
            time_feat_cpu.data[i] = time_sequences[cursors[i].first][cursors[i].second + step];
            time_label_cpu.data[i] = time_sequences[cursors[i].first][cursors[i].second + step + 1];
        }
        
        feat.CopyFrom(time_feat_cpu);
        label.CopyFrom(time_label_cpu);
    } 
   
    bool initialized;
    std::vector< std::pair<unsigned, unsigned> > cursors;                 
    std::vector< std::vector<int> > event_sequences;
    std::vector< std::vector<Dtype> > time_sequences;
    std::deque< unsigned > index_pool;
    SparseMat<CPU, Dtype> event_feat_cpu, event_label_cpu;
    DenseMat<CPU, Dtype> time_feat_cpu, time_label_cpu;
};


template<Phase phase>
class DataLoader; 

template<>
class DataLoader<TRAIN> : public IDataLoader
{
public:

    DataLoader(unsigned _num_events, unsigned _batch_size) : IDataLoader(_num_events, _batch_size)
    {
        
    }
    
    template<MatMode mode>             
    inline void NextBpttBatch(int bptt, GraphData<mode, Dtype>* g_last_hidden,
                              std::vector< GraphData<mode, Dtype>* >& g_event_input,
                              std::vector< GraphData<mode, Dtype>* >& g_time_input, 
                              std::vector< GraphData<mode, Dtype>* >& g_event_label,
                              std::vector< GraphData<mode, Dtype>* >& g_time_label)
    {
        if (!initialized)
            this->StartNewEpoch();
                        
        for (unsigned i = 0; i < this->batch_size; ++i)
        {
            // need to load a new sequences                                   
            if (cursors[i].second + bptt >= event_sequences[cursors[i].first].size())
            {                              
                this->ReloadSlot(g_last_hidden, i);                                                            
            }
        }
        for (int j = 0; j < bptt; ++j)
        {                                  
            this->LoadEvent(g_event_input[j], g_event_label[j], this->batch_size, j);                        
            this->LoadTime(g_time_input[j], g_time_label[j], this->batch_size, j);           
        }        
        for (unsigned i = 0; i < this->batch_size; ++i)
            cursors[i].second += bptt;           
    }
}; 

// assert batch size = 1 right now
template<>
class DataLoader<TEST> : public IDataLoader
{
public:
    
    DataLoader(unsigned _num_events, unsigned _batch_size) : IDataLoader(_num_events, _batch_size)
    {
        available.clear();
    }    
    
    template<MatMode mode>
    inline bool NextBatch(GraphData<mode, Dtype>* g_last_hidden,
                          GraphData<mode, Dtype>* g_event_input, 
                          GraphData<mode, Dtype>* g_time_input, 
                          GraphData<mode, Dtype>* g_event_label, 
                          GraphData<mode, Dtype>* g_time_label)
    {
        if (!this->initialized)
            this->StartNewEpoch();
            
        if (cursors[0].second + 1 >= event_sequences[cursors[0].first].size())
        {                                                          
            if (available[index_pool.front()])      
            {   
                available[index_pool.front()] = false;   
                this->ReloadSlot(g_last_hidden, 0);                
            }
            else
                return false;                                                                             
        }
        this->LoadEvent(g_event_input, g_event_label, 1, 0);
        this->LoadTime(g_time_input, g_time_label, 1, 0);   
        cursors[0].second++;                  
        return true;
    }

    virtual void StartNewEpoch() override
    {
        if (!this->initialized)
        {
            this->initialized = true;
            available.resize(event_sequences.size());                        
        }
        
        index_pool.clear();
        for (unsigned i = 0; i < event_sequences.size(); ++i)
        {
            index_pool.push_back(i);
            available[i] = true;
        }
               
        cursors[0].first = index_pool.front();
        cursors[0].second = 0;
        index_pool.pop_front();
        available[cursors[0].first] = false;                               
    }
    
protected:    
    
    std::vector<bool> available;     
}; 

DataLoader<TRAIN>* train_data;
DataLoader<TEST>* test_data;

std::vector< std::vector<int> > raw_event_data;
std::vector< std::vector<Dtype> > raw_time_data;

template<typename data_type>
inline void LoadRaw(const char* filename, std::vector< std::vector<data_type> >& raw_data)
{
    raw_data.clear();
    std::ifstream f_stream(filename);
    std::string read_buf;
    data_type d;
    while (getline(f_stream, read_buf))
    {
        std::stringstream ss(read_buf);
        std::vector<data_type> cur_seq;
        cur_seq.clear();
        while (ss >> d)
        {
            cur_seq.push_back(d); 
        }
        raw_data.push_back(cur_seq);
    }       
}

inline void ReadRawData()
{
    std::cerr << "loading data..." << std::endl;
    assert(cfg::f_time_data && cfg::f_event_data);    
    
    LoadRaw(cfg::f_event_data, raw_event_data);
    LoadRaw(cfg::f_time_data, raw_time_data);
    assert(raw_event_data.size() == raw_time_data.size());
    std::set<int> label_set;
    label_set.clear();
    for (unsigned i = 0; i < raw_event_data.size(); ++i)
    {
        for (unsigned j = 0; j < raw_event_data[i].size(); ++j)
            label_set.insert(raw_event_data[i][j]);
    }
    std::cerr << "totally " << label_set.size() << " events" << std::endl;
    train_data = new DataLoader<TRAIN>(label_set.size(), cfg::batch_size); 
    test_data = new DataLoader<TEST>(label_set.size(), 1);
    
    for (unsigned i = 0; i < raw_event_data.size(); ++i)
    {
        assert(raw_event_data[i].size() == raw_time_data[i].size());
        
        int origin_len = raw_event_data[i].size();
        int test_len = origin_len * 0.1;
        int train_len = origin_len - test_len;
        for (int j = 0; j < origin_len; ++j)
            raw_event_data[i][j]--; // the raw event is 1-based
        for (int j = origin_len - 1; j >= 1; --j)
            raw_time_data[i][j] = raw_time_data[i][j] - raw_time_data[i][j-1];            
        train_data->InsertSequence(raw_event_data[i].data(), raw_time_data[i].data(), train_len);
        test_data->InsertSequence(raw_event_data[i].data() + train_len, raw_time_data[i].data() + train_len, test_len);                             
    }
    std::cerr << raw_event_data.size() << " sequences loaded." << std::endl;
}

#endif