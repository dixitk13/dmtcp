// master node checks for existing znode and creates his data

#include <string.h>
#include <stdio.h>
#include <unistd.h>    
#include <zookeeper/zookeeper.h>


static zhandle_t *zh;
static int connected;
char gpath[1024];  
// static char mycontext[] = "testingdata";
char buffer[255]; 
int master_number = 999;    
char master_string[23];
struct String_vector vector; 
static char master_znode_base_path[] = "/master/dmtcp_coord_";



void get_shared_state(){
    printf("get shared state, implement me\n");
}

void zktest_string_completion (int RC, const  char * name, const  void * Data)
{
    if (! RC) {
        fprintf (stderr, "znode created =%s  " , name);
    }
}

void create_ephemeral_sequential_node(char *value){
    printf("creating node, please implement me\n");

    int rc = zoo_acreate(zh, master_znode_base_path, value, 14,
    &ZOO_OPEN_ACL_UNSAFE, ZOO_EPHEMERAL | ZOO_SEQUENCE, 
    zktest_string_completion, NULL);

    if(rc = ZOK){
        printf("creating a node with my data \n");
    }else{
        fprintf(stderr, "Error in creating znode %d\n", rc);  
         return;
    }
}

void initZooHandle(){

    zh = zookeeper_init("localhost:2181", NULL, 30000, 0, 0, 0);
    if(!zh){
        printf("error");
        return;
    }
    fprintf(stderr, "Opened zookeeper\n");  
}

void deleteZooHandle(){
    zookeeper_close(zh);
}


//create -s -e /master/dmtcp_coord_ "localhost:1119"
int main(int argc, char **argv){
    printf("hello zookeeper\n");
    static char value[] = "localhost:9219";

    initZooHandle();

    create_ephemeral_sequential_node(value);
   
    get_shared_state();

    printf("sleeping! \n");
    // do whatever master does
    while(1) sleep(1);  

    fprintf(stderr, "Closing zookeeper\n");  
    
    deleteZooHandle();
    return 0;
}
