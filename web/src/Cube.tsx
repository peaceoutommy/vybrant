import { useEffect } from 'react';
import { motion, useMotionValue, useSpring, useTransform } from 'motion/react';

const faces = ['front', 'back', 'right', 'left', 'top', 'bottom'];

// The mark on a glass cube that tumbles slowly and leans toward the pointer (styles.css: .cube)
export function Cube() {
  const px = useMotionValue(0), py = useMotionValue(0);   // pointer, -0.5..0.5 over the window
  const sx = useSpring(px, { stiffness: 50, damping: 16 }), sy = useSpring(py, { stiffness: 50, damping: 16 });
  const rotateX = useTransform(sy, [-0.5, 0.5], [22, -22]);
  const rotateY = useTransform(sx, [-0.5, 0.5], [-28, 28]);
  useEffect(() => {
    const move = (e: PointerEvent) => {
      px.set(e.clientX / window.innerWidth - 0.5);
      py.set(e.clientY / window.innerHeight - 0.5);
    };
    window.addEventListener('pointermove', move);
    return () => window.removeEventListener('pointermove', move);
  }, [px, py]);

  return (
    <motion.div
      className="cube-wrap"
      aria-hidden
      initial={{ opacity: 0, scale: 0.8 }}
      animate={{ opacity: 1, scale: 1 }}
      transition={{ duration: 1, ease: [0.22, 1, 0.36, 1] }}
    >
      <motion.div className="cube-tilt" style={{ rotateX, rotateY }}>
        <div className="cube">
          {faces.map((f) => (
            <span key={f} className={`face ${f}`} />
          ))}
        </div>
      </motion.div>
    </motion.div>
  );
}
