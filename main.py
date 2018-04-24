class ExecuteUnit(object):
    def __init__(self, params):
        pass
        #cmds = params.split()
        #print("cmds cmds[0]", cmds, cmds[0])
        #for item in cmds:
        #    print item
        
    def execute(self, cmds):
        print("BallDetection   cmd: ", cmds)
        return "BallDetection      cmd: ".join(str(x) for x in cmds)

class BallDetection(ExecuteUnit):
    def execute(self, cmds):
        print("BallDetection   cmd: ", cmds)
        return "BallDetection      cmd: ".join(str(x) for x in cmds)

class BallTracking(ExecuteUnit):
    def execute(self, cmds):
        print("BallTracking    cmd: ", cmds)
        return "BallTracking      cmd: ".join(str(x) for x in cmds)

class BallFusion(ExecuteUnit):
    def execute(self, cmds):
        print("BallFusion      cmd: ", cmds)
        return "BallFusion      cmd: ".join(str(x) for x in cmds)

class PlayerDetection(ExecuteUnit):
    def execute(self, cmds):
        print("PlayerDetection cmd: ", cmds)
        return "PlayerDetection cmd: ".join(str(x) for x in cmds)

class PlayerTracking(ExecuteUnit):
    def execute(self, cmds):
        print("PlayerTracking  cmd: ", cmds)
        return "PlayerTracking  cmd: ".join(str(x) for x in cmds)

class PlayerFusion(ExecuteUnit):
    def execute(self, cmds):
        print("PlayerFusion    cmd: ", cmds)
        return "PlayerFusion    cmd: ".join(str(x) for x in cmds)

class Encoder(ExecuteUnit):
    def execute(self, cmds):
        print("Encoder         cmd: ", cmds)
        return "Encoder         cmd: ".join(str(x) for  x in cmds)
    
def get_execute_unit(params):
    print("cmds cmds[0] ", params)
    ret = None
    cmds = params.split()
    print("cmds cmds[0]", cmds, cmds[0])
    for item in cmds:
        print item
    for i in range(0, len(cmds)):
        if cmds[i] == '-t':
            if cmds[i+1] == 'b':
                print(" ret ", cmds)
                ret = BallDetection(cmds)
            elif cmds[i+1] == 'tb':
                print(" ret ", cmds)
                ret = BallTracking(cmds)
            elif cmds[i+1] == 'fb':
                print(" ret ", cmds)
                ret = BallFusion(cmds)
            elif cmds[i+1] == 'p':
                print(" ret ", cmds)
                ret = PlayerDetection(cmds)
            elif cmds[i+1] == 'tp':
                print(" ret ", cmds)
                ret = PlayerTracking(cmds)
            elif cmds[i+1] == 'fp':
                print(" ret ", cmds)
                ret = PlayerFusion(cmds)
            elif cmds[i+1] == 'e':
                print(" ret ", cmds)
                ret = Encoder(cmds)
            break
    print(" ret ", ret)
    return ret
        
def exec_one_frame(exec_unit, frame_no, info, image):
    params = []
    if frame_no is not None and isinstance(frame_no, int) and frame_no >= 0:
        params.append("-f")
        params.append(frame_no)
    if info is not None and isinstance(info, str):
        params.append("-s")
        params.append(info)
    if image is not None:
        params.append("-img")
        params.append(image)
    else:
        print('image is none!')
    print(" call ", params)
    return exec_unit.execute(params)


def main():
    callback = get_execute_unit("-t b -i inputxxx -game xxxxxxxgame -live livexxxx --image_scale 0.5 -v 3 -v3")
    ret = exec_one_frame(callback, 1, "test info", "test image")
    print("main ret ", ret)


if __name__ == '__main__':
    main()
