#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include "parser.h"
//function for executing a command without a loop
//the paramaters are needed even if the STDIN_FILENO and STDOUT_FILENO are redirected,it is called by the loop pipe function.
void execute_pipeline_internal(CmdVec command_sequence, int pipeline_input_fd, int pipeline_output_fd){
    int num_commands=command_sequence.n;
    if(num_commands<0){
        return;
    }
    pid_t pids[num_commands];
    int input_fd_for_current_cmd=pipeline_input_fd;
    int output_fd_for_current_cmd;
    int p[2];
    for(int i=0;i<num_commands;i++){
        char **cmd_argv = command_sequence.argvs[i];
        if(i==num_commands-1){
           output_fd_for_current_cmd = pipeline_output_fd;
        }
        else{
            pipe(p);
            output_fd_for_current_cmd = p[1]; // Write-end of the pipe if it is not the last command
        }
        pids[i]=fork();
        if(pids[i]==0){
            if (input_fd_for_current_cmd != STDIN_FILENO) {
                    dup2(input_fd_for_current_cmd, STDIN_FILENO);
                    close(input_fd_for_current_cmd);
                }
            if (output_fd_for_current_cmd != STDOUT_FILENO) {
                    dup2(output_fd_for_current_cmd, STDOUT_FILENO);
                    close(output_fd_for_current_cmd);
                }
            //if it is not the last item it will not use the read end of the pipe.
            if (i < num_commands - 1) {
                    close(p[0]);
                } 
            execvp(cmd_argv[0], cmd_argv);    
            _exit(1);  
        }
        //parent process must close the copy of the pipe's ends if it's not the absolute files
        if (input_fd_for_current_cmd != pipeline_input_fd) {
            close(input_fd_for_current_cmd);
        }
        if (output_fd_for_current_cmd != pipeline_output_fd) {
            close(output_fd_for_current_cmd);
        }
        //plumbing for the next iteration.It must make the current input be the read end of the pipe
        if (i < num_commands - 1) {
            input_fd_for_current_cmd = p[0];
        }
    }
    //parent waits for fork calls to finish
    for (int i = 0; i < num_commands; i++) {
        waitpid(pids[i], NULL, 0);
    }
        }
//function to execute internal logic n times,ite creates outer and internal pipes and the function itself cleans the outer pipe and readies it for next iteration
void execute_loop_pipe(CmdVec pipeline_to_loop,int n_iterations){
    int input_for_iteration = STDIN_FILENO;
    int output_for_iteration;
    int p[2];
    for(int i=0;i<n_iterations;i++){
       int is_last_iteration = (i == n_iterations - 1); 
       if(is_last_iteration){
        output_for_iteration=STDOUT_FILENO;
       }
       else{
        pipe(p);
        output_for_iteration=p[1];
       }
       execute_pipeline_internal(pipeline_to_loop, input_for_iteration, output_for_iteration);
       if(input_for_iteration!=STDIN_FILENO){
        close(input_for_iteration);
       }
       if(output_for_iteration!=STDOUT_FILENO){
        close(output_for_iteration);
       }
       if(!is_last_iteration){
        input_for_iteration=p[0];
       }
    }
}

typedef enum { STAGE_SIMPLE, STAGE_LOOP } StageType;
typedef struct {
    StageType type;
    CmdVec commands;
    size_t loopLen;
} PipelineStage;

void execute_command(compiledCmd C){
    int final_input_fd = STDIN_FILENO;
    int final_output_fd = STDOUT_FILENO;  
    if(C.inFile!=NULL){
        final_input_fd=open(C.inFile, O_RDONLY);
    }
    if(C.outFile!=NULL){
       final_output_fd = open(C.outFile, O_WRONLY | O_CREAT | O_TRUNC, 0644); 
    }
    PipelineStage stages_to_run[3];
    int num_stages = 0;
    if (C.before.n > 0) {
        stages_to_run[num_stages].type = STAGE_SIMPLE;
        stages_to_run[num_stages].commands = C.before;
        stages_to_run[num_stages].loopLen = 0;
        num_stages++;
    }
    
    if (C.loopLen > 0 && C.inLoop.n > 0) { 
        stages_to_run[num_stages].type = STAGE_LOOP;
        stages_to_run[num_stages].commands = C.inLoop;
        stages_to_run[num_stages].loopLen = C.loopLen;
        num_stages++;
    }
    
    if (C.after.n > 0) {
        stages_to_run[num_stages].type = STAGE_SIMPLE;
        stages_to_run[num_stages].commands = C.after;
        stages_to_run[num_stages].loopLen = 0;
        num_stages++;
    }
   if (num_stages == 0) {
        if (final_input_fd != STDIN_FILENO) close(final_input_fd);
        if (final_output_fd != STDOUT_FILENO) close(final_output_fd);
        return;
    }
    pid_t pids[num_stages];
    int input_fd_for_current_cmd = final_input_fd;
    int output_fd_for_current_cmd;
    int p[2];
    for(int i=0;i<num_stages;i++){
        if(i==num_stages-1){
            output_fd_for_current_cmd=final_output_fd;
        }
        else{
            pipe(p);
            output_fd_for_current_cmd=p[1];
        }
        pids[i]=fork();
        if(pids[i]==0){
            if(input_fd_for_current_cmd!=STDIN_FILENO){
                dup2(input_fd_for_current_cmd,STDIN_FILENO);
                close(input_fd_for_current_cmd);
            }
            if(output_fd_for_current_cmd!=STDOUT_FILENO){
                dup2(output_fd_for_current_cmd,STDOUT_FILENO);
                close(output_fd_for_current_cmd);
            }
            if(i<num_stages-1){
                close(p[0]);
            }
           PipelineStage *my_stage = &stages_to_run[i];
           if (my_stage->type == STAGE_LOOP) {
                execute_loop_pipe(my_stage->commands, my_stage->loopLen);
            } else {
                execute_pipeline_internal(my_stage->commands, STDIN_FILENO, STDOUT_FILENO);
            }

            _exit(0);
        }
        if (input_fd_for_current_cmd != final_input_fd) {
            close(input_fd_for_current_cmd);
        }
        if (output_fd_for_current_cmd != final_output_fd) {
            close(output_fd_for_current_cmd);
        }
        if (i < num_stages - 1) {
            input_fd_for_current_cmd = p[0];
        }
    } 
    for (int i = 0; i < num_stages; i++) {
        waitpid(pids[i], NULL, 0);
    }
    if (final_input_fd != STDIN_FILENO) {
        close(final_input_fd);
    }
    if (final_output_fd != STDOUT_FILENO) {
        close(final_output_fd);
    } 

}
int main(void) {
    
    sparser_t parser;
    initParser(&parser);
    
    char *line = NULL;
    size_t len = 0;
    ssize_t nread;
    compiledCmd C;

    while (1) {
        printf("SUShell$ ");
        fflush(stdout);

        nread = getline(&line, &len, stdin);
        if (nread == -1) {
            printf("\n");
            break;
        }

        compileCommand(&parser, line, &C);

        if (C.isQuit) {
            printf("Exiting shell...\n");
            freeCompiledCmd(&C);
            free(line);
            break;
        }

        execute_command(C);

        freeCompiledCmd(&C);

    }
    freeParser(&parser);
    return 0;
}
